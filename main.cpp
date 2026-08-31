// deepin-liferaft: macOS "Your system has run out of application memory" 对话框的 DTK 克隆
// Fedora systemd-oomd 策略触发 → 自动弹出，按 DDE 应用 cgroup 内存排序。
#include <DApplication>
#include <DLog>
#include <DMainWindow>
#include <DTitlebar>
#include <DPushButton>
#include <DSuggestButton>
#include <DLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QIcon>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QHash>
#include <QElapsedTimer>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSettings>
#include <QLocale>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

DWIDGET_USE_NAMESPACE

static const double PRESSURE_LIMIT = 50.0; // Fedora user@.service policy
static const int PRESSURE_DURATION_MS = 20000;
static const int RECLAIM_WINDOW_MS = 30000;
static const int SWAP_USED_LIMIT = 90;
static const int SWAP_CANDIDATE_LIMIT = 5;
static const int POST_ACTION_DELAY_MS = 15000;
static const int POLL_MS = 1000;

enum class Trigger { None, Pressure, Swap };

struct Proc {
    QString cgroup;
    quint64 memory; // bytes
    quint64 swap;
    std::optional<quint64> pgscan;
    quint64 reclaim = 0;
    QString name;
    QString icon;
};

struct SystemMemory {
    quint64 memTotal = 0;
    quint64 memAvailable = 0;
    quint64 swapTotal = 0;
    quint64 swapFree = 0;
    bool valid = false;
};

static QString userCgroupPath() {
    return QString("/sys/fs/cgroup/user.slice/user-%1.slice/user@%1.service").arg(getuid());
}

static double parseFullAvg10(const QByteArray &data) {
    for (const auto &line : data.split('\n'))
        if (line.startsWith("full"))
            for (const auto &field : line.split(' '))
                if (field.startsWith("avg10="))
                    return field.mid(6).toDouble();
    return -1;
}

static double psiFullAvg10() {
    QFile f(userCgroupPath() + "/memory.pressure");
    return f.open(QIODevice::ReadOnly) ? parseFullAvg10(f.readAll()) : -1;
}

static std::optional<quint64> fileValue(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    bool ok;
    const quint64 value = f.readAll().trimmed().toULongLong(&ok);
    return ok ? std::optional<quint64>(value) : std::nullopt;
}

static std::optional<quint64> memoryStatValue(const QString &cgroup, const QByteArray &key) {
    QFile f(cgroup + "/memory.stat");
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    for (const auto &line : f.readAll().split('\n')) {
        if (!line.startsWith(key + ' ')) continue;
        bool ok;
        const quint64 value = line.mid(key.size() + 1).toULongLong(&ok);
        return ok ? std::optional<quint64>(value) : std::nullopt;
    }
    return std::nullopt;
}

static SystemMemory systemMemory() {
    SystemMemory out;
    unsigned seen = 0;
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly)) return out;
    for (const auto &line : f.readAll().split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2) continue;
        bool ok;
        const quint64 value = fields[1].toULongLong(&ok);
        if (!ok || value > std::numeric_limits<quint64>::max() / 1024) continue;
        const quint64 bytes = value * 1024;
        if (line.startsWith("MemTotal:")) { out.memTotal = bytes; seen |= 1; }
        else if (line.startsWith("MemAvailable:")) { out.memAvailable = bytes; seen |= 2; }
        else if (line.startsWith("SwapTotal:")) { out.swapTotal = bytes; seen |= 4; }
        else if (line.startsWith("SwapFree:")) { out.swapFree = bytes; seen |= 8; }
    }
    out.valid = seen == 15 && out.memAvailable <= out.memTotal && out.swapFree <= out.swapTotal;
    return out;
}

static bool percentAbove(quint64 used, quint64 total, int limit) {
    return total > 0 && used * 100 > total * quint64(limit);
}

static bool systemSwapPressure(const SystemMemory &memory) {
    if (!memory.valid) return false;
    return percentAbove(memory.memTotal - memory.memAvailable, memory.memTotal, SWAP_USED_LIMIT)
        && percentAbove(memory.swapTotal - memory.swapFree, memory.swapTotal, SWAP_USED_LIMIT);
}

static QString unescapeUnit(QString text) {
    for (int i = 0; i + 3 < text.size(); ++i) {
        if (text[i] != '\\' || text[i + 1] != 'x') continue;
        bool ok;
        const ushort value = text.mid(i + 2, 2).toUShort(&ok, 16);
        if (ok) text.replace(i, 4, QChar(value));
    }
    return text;
}

static std::optional<QString> appIdFromUnit(const QString &unit) {
    QString appId;
    if (unit.startsWith("app-DDE-")) {
        appId = unit.mid(8).section('@', 0, 0);
    } else if (unit.startsWith("app-") && unit.endsWith(".scope")) {
        appId = unit.mid(4, unit.size() - 10);
        const int separator = appId.lastIndexOf('-');
        bool pidOk = false;
        appId.mid(separator + 1).toUInt(&pidOk);
        if (separator <= 0 || !pidOk) return std::nullopt;
        appId.truncate(separator);
    } else {
        return std::nullopt;
    }
    appId = unescapeUnit(appId);
    if (appId.endsWith(".autostart")) appId.chop(10);
    return appId;
}

static QStringList desktopInfo(const QString &appId) {
    static QHash<QString, QStringList> cache;
    if (cache.contains(appId)) return cache.value(appId);

    QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
                                           appId + ".desktop");
    if (file.isEmpty()) {
        const QString linglong = "/var/lib/linglong/entries/apps/share/applications/" + appId + ".desktop";
        if (QFile::exists(linglong)) file = linglong;
    }
    if (file.isEmpty()) return cache.insert(appId, {}).value();
    QSettings desktop(file, QSettings::IniFormat);
    desktop.beginGroup("Desktop Entry");
    const QString locale = QLocale::system().name();
    QString name = desktop.value("Name[" + locale + "]").toString();
    if (name.isEmpty()) name = desktop.value("Name[" + locale.section('_', 0, 0) + "]").toString();
    if (name.isEmpty()) name = desktop.value("Name").toString();
    return cache.insert(appId, {name, desktop.value("Icon").toString()}).value();
}

// Whitelist entries are desktop file IDs (the same ID parsed from the cgroup
// unit name), one per line; blank lines and '#' comments are ignored.
static QSet<QString> parseWhitelist(const QByteArray &data) {
    QSet<QString> out;
    for (const auto &raw : data.split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        out.insert(QString::fromUtf8(line));
    }
    return out;
}

// vim-style levels, merged as one set: package defaults under
// /usr/share/deepin-liferaft, administrator entries under
// /etc/deepin-liferaft, and per-user entries under ~/.config.
static QStringList whitelistPaths() {
    QStringList paths;
    for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::AppDataLocation))
        paths << dir + "/whitelist";
    paths << "/etc/deepin-liferaft/whitelist";
    const QString userConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!userConfig.isEmpty()) paths << userConfig + "/whitelist";
    return paths;
}

static QSet<QString> loadWhitelist(const QStringList &paths) {
    QSet<QString> out;
    for (const auto &path : paths) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) out.unite(parseWhitelist(file.readAll()));
    }
    return out;
}

static QString ownCgroupPath() {
    QFile f("/proc/self/cgroup");
    if (!f.open(QIODevice::ReadOnly)) return {};
    for (const auto &line : f.readAll().split('\n'))
        if (line.startsWith("0::")) return "/sys/fs/cgroup" + QString::fromUtf8(line.mid(3)).trimmed();
    return {};
}

static bool writeCgroup(const QString &path, const char *file, const char *value) {
    QFile control(path + "/" + file);
    return control.open(QIODevice::WriteOnly) && control.write(value) > 0;
}

static bool freezeOwned(const QString &cgroup, QSet<QString> &owned) {
    const auto state = fileValue(cgroup + "/cgroup.freeze");
    if (!state || *state != 0 || !writeCgroup(cgroup, "cgroup.freeze", "1")) return false;
    owned.insert(cgroup);
    return true;
}

static bool thawCgroup(const QString &cgroup) {
    if (!QFile::exists(cgroup)) return true;
    const auto state = fileValue(cgroup + "/cgroup.freeze");
    return state && (*state == 0 || writeCgroup(cgroup, "cgroup.freeze", "0"));
}

static bool thawOwned(QSet<QString> &owned) {
    QSet<QString> failed;
    for (const auto &cgroup : owned)
        if (!thawCgroup(cgroup)) failed.insert(cgroup);
    owned = failed;
    return owned.isEmpty();
}

static int createSignalFd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) return -1;
    const int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    return fd;
}

// DDE 启动组和应用自建 scope 都是独立的应用资源边界。
static std::optional<QList<Proc>> appProcs(const QString &root, bool requireSwap, bool requirePgscan,
                                           const QSet<QString> &whitelist) {
    QList<Proc> out;
    const QDir appRoot(root);
    if (!appRoot.exists()) return std::nullopt;
    for (const auto &unit : appRoot.entryList({"app-DDE-*", "app-*.scope"},
                                              QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto appId = appIdFromUnit(unit);
        if (!appId || whitelist.contains(*appId)) continue;
        const QString cgroup = root + "/" + unit;
        const auto memory = fileValue(cgroup + "/memory.current");
        const auto swap = fileValue(cgroup + "/memory.swap.current");
        const auto pgscan = memoryStatValue(cgroup, "pgscan");
        if (!memory || (requireSwap && !swap) || (requirePgscan && !pgscan)) return std::nullopt;
        if (!*memory) continue;

        const auto desktop = desktopInfo(*appId);
        QString name = desktop.value(0);
        if (name.isEmpty()) name = *appId;
        out << Proc{cgroup,
                    *memory,
                    swap.value_or(0),
                    pgscan,
                    0,
                    name,
                    desktop.value(1, *appId)};
    }
    return out;
}

static quint64 pgscanDelta(const std::optional<quint64> &previous,
                           const std::optional<quint64> &current) {
    return previous && current && *current >= *previous ? *current - *previous : 0;
}

static bool hasSwapCandidate(const QList<Proc> &apps, quint64 swapTotal) {
    if (!swapTotal) return false;
    const quint64 threshold = swapTotal * SWAP_CANDIDATE_LIMIT / 100;
    return std::any_of(apps.cbegin(), apps.cend(),
                       [threshold](const Proc &app) { return app.swap > threshold; });
}

static Trigger selectTrigger(double pressure, qint64 pressureDuration, bool recentReclaim,
                             const SystemMemory &memory, const QList<Proc> &apps) {
    if (systemSwapPressure(memory) && hasSwapCandidate(apps, memory.swapTotal)) return Trigger::Swap;
    if (pressure > PRESSURE_LIMIT && pressureDuration >= PRESSURE_DURATION_MS && recentReclaim)
        return Trigger::Pressure;
    return Trigger::None;
}

static QString fmtSize(quint64 bytes) {
    if (bytes >= (1ull << 30)) return QString::number(bytes / 1073741824.0, 'f', 1) + " GB";
    return QString::number(bytes / 1048576.0, 'f', 1) + " MB";
}

static QString triggerName(Trigger trigger) {
    switch (trigger) {
    case Trigger::Swap: return QStringLiteral("swap");
    case Trigger::Pressure: return QStringLiteral("pressure");
    case Trigger::None: return QStringLiteral("none");
    }
    return QStringLiteral("unknown");
}

static bool freezerSelfTest() {
    QTemporaryDir dir;
    const QString app = dir.path() + "/app";
    const QString missing = dir.path() + "/missing";
    if (!dir.isValid() || !QDir().mkpath(app) || !QDir().mkpath(missing)) return false;
    QFile state(app + "/cgroup.freeze");
    if (!state.open(QIODevice::WriteOnly) || state.write("0") != 1) return false;
    state.close();

    QSet<QString> owned;
    if (!freezeOwned(app, owned) || fileValue(app + "/cgroup.freeze") != 1) return false;
    if (!thawOwned(owned) || fileValue(app + "/cgroup.freeze") != 0) return false;

    if (!state.open(QIODevice::WriteOnly) || state.write("1") != 1) return false;
    state.close();
    if (freezeOwned(app, owned) || !owned.isEmpty()) return false;

    owned.insert(missing);
    return !thawOwned(owned) && owned.contains(missing);
}

static bool whitelistSelfTest() {
    QTemporaryDir dir;
    if (!dir.isValid()) return false;
    const QString root = dir.path() + "/app.slice";
    const QString allowed = root + "/app-DDE-allowed@1000.service";
    const QString blocked = root + "/app-DDE-blocked@1001.service";
    if (!QDir().mkpath(allowed) || !QDir().mkpath(blocked)) return false;
    for (const auto &cgroup : {allowed, blocked}) {
        QFile current(cgroup + "/memory.current");
        if (!current.open(QIODevice::WriteOnly) || current.write("1024") <= 0) return false;
    }

    const auto all = appProcs(root, false, false, {});
    if (!all || all->size() != 2) return false;
    const auto filtered = appProcs(root, false, false, {"blocked"});
    if (!filtered || filtered->size() != 1 || filtered->first().name != "allowed") return false;

    const auto parsed = parseWhitelist("# comment\n\nfoo-bar\n  baz \t\n");
    if (parsed != QSet<QString>({"foo-bar", "baz"})) return false;

    QFile systemFile(dir.path() + "/system.list");
    if (!systemFile.open(QIODevice::WriteOnly) || systemFile.write("alpha\n# note\n") <= 0)
        return false;
    systemFile.close();
    QFile userFile(dir.path() + "/user.list");
    if (!userFile.open(QIODevice::WriteOnly) || userFile.write("beta\nalpha\n") <= 0)
        return false;
    userFile.close();
    const auto merged = loadWhitelist({systemFile.fileName(), dir.path() + "/missing", userFile.fileName()});
    return merged == QSet<QString>({"alpha", "beta"});
}

static bool selfTest() {
    const SystemMemory highSwap{100, 9, 100, 9, true};
    const SystemMemory atLimit{100, 10, 100, 10, true};
    const QList<Proc> swapCandidate{{{}, 1, 6, 0, 0, {}, {}}};
    const QList<Proc> exactSwapLimit{{{}, 1, 5, 0, 0, {}, {}}};
    return unescapeUnit("google\\x2dchrome") == "google-chrome"
        && appIdFromUnit("app-DDE-google\\x2dchrome@123.service") == "google-chrome"
        && appIdFromUnit("app-code-113545.scope") == "code"
        && !appIdFromUnit("app-code.scope")
        && parseFullAvg10("some avg10=99.00 avg60=1.00\nfull avg10=50.25 avg60=2.00\n") == 50.25
        && selectTrigger(50.0, 20000, true, {}, {}) == Trigger::None
        && selectTrigger(50.1, 19999, true, {}, {}) == Trigger::None
        && selectTrigger(50.1, 20000, false, {}, {}) == Trigger::None
        && selectTrigger(50.1, 20000, true, {}, {}) == Trigger::Pressure
        && selectTrigger(0, 0, false, highSwap, swapCandidate) == Trigger::Swap
        && selectTrigger(0, 0, false, atLimit, swapCandidate) == Trigger::None
        && selectTrigger(0, 0, false, highSwap, exactSwapLimit) == Trigger::None
        && selectTrigger(0, 0, false, highSwap, {}) == Trigger::None
        && pgscanDelta(100, 101) == 1
        && pgscanDelta(100, std::nullopt) == 0
        && pgscanDelta(std::nullopt, 101) == 0
        && freezerSelfTest()
        && whitelistSelfTest()
        && fmtSize(56727962) == "54.1 MB"
        && fmtSize(1610612736) == "1.5 GB";
}

static const int APP_NAME_ROLE = Qt::UserRole + 1;
static const int PAUSED_ROLE = Qt::UserRole + 2;

class AppNameDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem base(option);
        initStyleOption(&base, index);
        const QIcon icon = base.icon;
        base.icon = {};
        base.text.clear();
        QStyle *style = base.widget ? base.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &base, painter, base.widget);

        painter->save();
        painter->setClipRect(option.rect);
        const QRect iconRect(option.rect.left() + 4, option.rect.center().y() - 12, 24, 24);
        icon.paint(painter, iconRect);

        const bool paused = index.data(PAUSED_ROLE).toBool();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QColor normal = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const QFontMetrics metrics(option.font);
        const QString suffix = paused ? QCoreApplication::translate("AppNameDelegate", "(Paused)") : QString();
        const int suffixWidth = metrics.horizontalAdvance(suffix);
        const int textX = iconRect.right() + 7;
        const int available = std::max(0, option.rect.right() - textX - suffixWidth - 4);
        const QString name = metrics.elidedText(index.data(APP_NAME_ROLE).toString(),
                                                 Qt::ElideRight, available);
        const int baseline = option.rect.center().y() + (metrics.ascent() - metrics.descent()) / 2;

        painter->setPen(paused ? QColor(205, 45, 45) : normal);
        painter->drawText(textX, baseline, name);
        if (paused) {
            painter->setPen(normal);
            painter->drawText(textX + metrics.horizontalAdvance(name), baseline, suffix);
        }
        painter->restore();
    }
};

class LiferaftApplication : public DApplication {
public:
    using DApplication::DApplication;

    void setQuitGuard(std::function<bool()> guard) {
        m_quitGuard = std::move(guard);
    }

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::Quit && m_quitGuard && !m_quitGuard()) {
            if (!m_quitRetryScheduled) {
                m_quitRetryScheduled = true;
                QTimer::singleShot(250, this, [this] {
                    m_quitRetryScheduled = false;
                    QCoreApplication::quit();
                });
            }
            return true;
        }
        return DApplication::event(event);
    }

private:
    std::function<bool()> m_quitGuard;
    bool m_quitRetryScheduled = false;
};

class ForceQuitWindow : public DMainWindow {
    Q_OBJECT
public:
    explicit ForceQuitWindow(int signalFd, QSet<QString> whitelist)
        : m_whitelist(std::move(whitelist)) {
        setWindowTitle(tr("Force Quit Applications"));
        setFixedSize(520, 460);

        if (const auto pgscan = memoryStatValue(userCgroupPath(), "pgscan")) {
            m_lastUserPgscan = *pgscan;
            m_hasLastUserPgscan = true;
        }

        if (signalFd >= 0) {
            auto *notifier = new QSocketNotifier(signalFd, QSocketNotifier::Read, this);
            connect(notifier, &QSocketNotifier::activated, this, [this, signalFd] {
                signalfd_siginfo info;
                while (::read(signalFd, &info, sizeof(info)) == sizeof(info)) {}
                requestShutdown();
            });
        }

        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this] { poll(); });
        m_timer->start(POLL_MS);
    }

    void ensureUi() {
        if (m_table) return;


        const QIcon icon(":/icons/deepin-liferaft.svg");
        setWindowIcon(icon);
        titlebar()->setIcon(icon);
        titlebar()->setTitle(windowTitle());
        auto *central = new QWidget;
        auto *vbox = new QVBoxLayout(central);
        vbox->setContentsMargins(24, 20, 24, 20);
        vbox->setSpacing(10);

        auto *heading = new QHBoxLayout;
        auto *warning = new DLabel;
        warning->setFixedSize(54, 54);
        warning->setAlignment(Qt::AlignCenter);
        warning->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
        heading->addWidget(warning, 0, Qt::AlignTop);

        auto *headingText = new QVBoxLayout;
        auto *title = new DLabel(tr("Your system has run out of application memory."));
        QFont font = title->font();
        font.setPointSize(15);
        font.setBold(true);
        title->setFont(font);
        title->setWordWrap(true);
        title->setElideMode(Qt::ElideNone);
        auto *sub = new DLabel(tr("To avoid problems with your computer, quit applications you are no longer using."));
        sub->setWordWrap(true);
        sub->setElideMode(Qt::ElideNone);
        QPalette palette = sub->palette();
        palette.setColor(QPalette::WindowText, QColor(80, 80, 85));
        sub->setPalette(palette);
        headingText->addWidget(title);
        headingText->addWidget(sub);
        heading->addLayout(headingText, 1);
        vbox->addLayout(heading);
        vbox->addSpacing(6);

        m_table = new QTableWidget(0, 2);
        m_table->horizontalHeader()->hide();
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_table->verticalHeader()->hide();
        m_table->verticalHeader()->setDefaultSectionSize(30);
        m_table->setItemDelegateForColumn(0, new AppNameDelegate(m_table));
        m_table->setShowGrid(false);
        m_table->setFrameShape(QFrame::StyledPanel);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setFocusPolicy(Qt::NoFocus);
        QPalette tablePal = m_table->palette();
        tablePal.setColor(QPalette::Inactive, QPalette::Highlight,
                          tablePal.color(QPalette::Active, QPalette::Highlight));
        tablePal.setColor(QPalette::Inactive, QPalette::HighlightedText,
                          tablePal.color(QPalette::Active, QPalette::HighlightedText));
        m_table->setPalette(tablePal);
        m_table->setIconSize(QSize(24, 24));
        vbox->addWidget(m_table);

        auto *buttons = new QHBoxLayout;
        m_resumeBtn = new DPushButton(tr("Resume"));
        m_killBtn = new DSuggestButton;
        m_killBtn->setText(tr("Force Quit"));
        buttons->addStretch();
        buttons->addWidget(m_resumeBtn);
        buttons->addWidget(m_killBtn);
        vbox->addLayout(buttons);
        setCentralWidget(central);

        connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(m_resumeBtn, &DPushButton::clicked, this, [this] {
            const int row = m_table->currentRow();
            if (row < 0) return;
            const QString cgroup = m_table->item(row, 0)->data(Qt::UserRole).toString();
            if (thawCgroup(cgroup)) {
                m_frozen.remove(cgroup);
                qInfo() << tr("Resumed %1").arg(cgroup);
            } else {
                qWarning() << tr("Resume failed for %1").arg(cgroup);
            }
            sampleApps();
            refresh();
        });
        connect(m_killBtn, &DPushButton::clicked, this, [this] {
            const int row = m_table->currentRow();
            if (row < 0) return;
            const QString cgroup = m_table->item(row, 0)->data(Qt::UserRole).toString();
            const bool owned = m_frozen.contains(cgroup);
            const bool killed = writeCgroup(cgroup, "cgroup.kill", "1");
            const bool thawed = !owned || thawCgroup(cgroup);
            if (killed && thawed) {
                m_frozen.remove(cgroup);
                qInfo() << tr("Force quit %1: kill=%2 thaw=%3").arg(cgroup).arg(killed).arg(thawed);
            } else {
                qWarning() << tr("Force quit %1 failed: kill=%2 thaw=%3").arg(cgroup).arg(killed).arg(thawed);
            }
            sampleApps();
            refresh();
        });
    }

    void releaseUi() {
        if (!m_table) return;
        QWidget *central = takeCentralWidget();
        m_table = nullptr;
        m_resumeBtn = nullptr;
        m_killBtn = nullptr;
        central->deleteLater();
    }

    bool sampleApps(bool requireSwap = false, bool requirePgscan = false) {
        const auto snapshot = appProcs(userCgroupPath() + "/app.slice",
                                       requireSwap, requirePgscan, m_whitelist);
        if (!snapshot) {
            if (requirePgscan) m_lastPgscan.clear();
            return false;
        }
        auto apps = *snapshot;
        QHash<QString, quint64> current;
        for (auto &app : apps) {
            const auto previous = m_lastPgscan.constFind(app.cgroup);
            app.reclaim = pgscanDelta(previous == m_lastPgscan.cend()
                                          ? std::nullopt
                                          : std::optional<quint64>(previous.value()),
                                      app.pgscan);
            if (app.pgscan) current.insert(app.cgroup, *app.pgscan);
        }
        for (const auto &old : m_apps)
            if (m_frozen.contains(old.cgroup)
                && std::none_of(apps.cbegin(), apps.cend(), [&old](const Proc &app) {
                       return app.cgroup == old.cgroup;
                   }))
                apps << old;
        m_lastPgscan = current;
        m_apps = apps;
        return true;
    }

    void poll() {
        bool userPgscanValid = false;
        if (const auto pgscan = memoryStatValue(userCgroupPath(), "pgscan")) {
            if (m_hasLastUserPgscan && *pgscan > m_lastUserPgscan) m_reclaimSeen.start();
            m_lastUserPgscan = *pgscan;
            m_hasLastUserPgscan = true;
            userPgscanValid = true;
        } else
            m_hasLastUserPgscan = false;

        const double pressure = psiFullAvg10();
        if (pressure > PRESSURE_LIMIT) {
            if (!m_pressureSince.isValid()) {
                m_pressureSince.start();
                qInfo() << tr("Memory pressure above limit: %1%").arg(pressure, 0, 'f', 1);
            }
        } else {
            if (m_pressureSince.isValid())
                qInfo() << tr("Memory pressure back below limit: %1%").arg(pressure, 0, 'f', 1);
            m_pressureSince.invalidate();
        }

        const SystemMemory memory = systemMemory();
        const bool pressureSampling = pressure > PRESSURE_LIMIT;
        const bool swapSampling = systemSwapPressure(memory);
        bool appSampleValid = true;
        if (pressureSampling || swapSampling || isVisible())
            appSampleValid = sampleApps(swapSampling, pressureSampling);

        const bool recentReclaim = m_reclaimSeen.isValid()
            && m_reclaimSeen.elapsed() <= RECLAIM_WINDOW_MS;
        const qint64 pressureDuration = m_pressureSince.isValid() ? m_pressureSince.elapsed() : -1;
        const bool samplesValid = pressure >= 0 && memory.valid && userPgscanValid && appSampleValid;
        if (!samplesValid) {
            if (!m_lastSamplesInvalid) {
                m_lastSamplesInvalid = true;
                qWarning() << tr("Invalid samples: pressure=%1 memory=%2 userPgscan=%3 appSample=%4")
                             .arg(pressure >= 0).arg(memory.valid).arg(userPgscanValid).arg(appSampleValid);
            }
        } else
            m_lastSamplesInvalid = false;
        const Trigger trigger = samplesValid
            ? selectTrigger(pressure, pressureDuration, recentReclaim, memory, m_apps)
            : Trigger::None;
        const bool coolingDown = m_postAction.isValid() && m_postAction.elapsed() < POST_ACTION_DELAY_MS;

        if (trigger != Trigger::None && !coolingDown && !isVisible()) {
            qInfo() << tr("Trigger %1: pressure=%2%% duration=%3ms recentReclaim=%4 "
                     "memUsed=%5%% swapUsed=%6%% apps=%7")
                      .arg(triggerName(trigger))
                      .arg(pressure, 0, 'f', 1)
                      .arg(pressureDuration)
                      .arg(recentReclaim)
                      .arg(memory.memTotal ? (memory.memTotal - memory.memAvailable) * 100 / memory.memTotal : 0)
                      .arg(memory.swapTotal ? (memory.swapTotal - memory.swapFree) * 100 / memory.swapTotal : 0)
                      .arg(m_apps.size());
            refresh();
            freezeCandidates(trigger, memory.swapTotal);
            m_postAction.start();
            show();
            raise();
            activateWindow();
        } else if (isVisible())
            refresh();
    }

    void freezeCandidates(Trigger trigger, quint64 swapTotal) {
        auto candidates = m_apps;
        if (trigger == Trigger::Swap)
            std::sort(candidates.begin(), candidates.end(),
                      [](const Proc &a, const Proc &b) { return a.swap > b.swap; });
        else
            std::sort(candidates.begin(), candidates.end(), [](const Proc &a, const Proc &b) {
                return a.reclaim == b.reclaim ? a.memory > b.memory : a.reclaim > b.reclaim;
            });

        const quint64 swapThreshold = swapTotal * SWAP_CANDIDATE_LIMIT / 100;
        const QString ownCgroup = ownCgroupPath();
        if (ownCgroup.isEmpty()) return;
        int frozen = 0;
        for (const auto &app : candidates) {
            if (trigger == Trigger::Swap && app.swap <= swapThreshold) continue;
            if (ownCgroup == app.cgroup || ownCgroup.startsWith(app.cgroup + '/')) continue;
            if (m_frozen.contains(app.cgroup)) continue;
            if (freezeOwned(app.cgroup, m_frozen)) {
                qInfo() << tr("Frozen %1 (%2) trigger=%3 reclaim=%4MB swap=%5MB memory=%6MB")
                          .arg(app.name)
                          .arg(app.cgroup)
                          .arg(triggerName(trigger))
                          .arg(app.reclaim / 1048576.0, 0, 'f', 1)
                          .arg(app.swap / 1048576.0, 0, 'f', 1)
                          .arg(app.memory / 1048576.0, 0, 'f', 1);
                if (++frozen == 3) break;
            } else {
                qWarning() << tr("Cannot freeze %1 (%2): unreadable or frozen by another component")
                             .arg(app.name)
                             .arg(app.cgroup);
            }
        }
        refresh();
    }

    bool unfreezeAll() {
        return thawOwned(m_frozen);
    }

    void requestShutdown() {
        if (m_shutdownRequested) return;
        m_shutdownRequested = true;
        qInfo() << tr("Shutdown requested, thawing %1 frozen cgroup(s)").arg(m_frozen.size());
        retryShutdown();
    }

    void retryShutdown() {
        if (unfreezeAll()) {
            qInfo() << tr("All frozen cgroups thawed, exiting");
            qApp->quit();
        } else {
            qWarning() << tr("Thaw incomplete (%1 cgroup(s) remain), retrying").arg(m_frozen.size());
            QTimer::singleShot(250, this, [this] { retryShutdown(); });
        }
    }

    void updateButtons() {
        if (!m_table) return;
        const int row = m_table->currentRow();
        const QString cgroup = row < 0 ? QString() : m_table->item(row, 0)->data(Qt::UserRole).toString();
        m_resumeBtn->setEnabled(!cgroup.isEmpty() && m_frozen.contains(cgroup));
        m_killBtn->setEnabled(!cgroup.isEmpty());
    }

    void refresh() {
        if (!m_table) return;
        QString selected;
        if (m_table->currentRow() >= 0)
            selected = m_table->item(m_table->currentRow(), 0)->data(Qt::UserRole).toString();

        auto procs = m_apps;
        std::sort(procs.begin(), procs.end(), [this](const Proc &a, const Proc &b) {
            const bool aFrozen = m_frozen.contains(a.cgroup);
            const bool bFrozen = m_frozen.contains(b.cgroup);
            return aFrozen == bFrozen ? a.memory > b.memory : aFrozen;
        });
        if (procs.size() > 10) procs.resize(10);
        m_table->setRowCount(procs.size());
        int selectedRow = -1;
        for (int i = 0; i < procs.size(); ++i) {
            const bool frozen = m_frozen.contains(procs[i].cgroup);
            auto *name = new QTableWidgetItem(procs[i].name + (frozen ? tr("(Paused)") : QString()));
            name->setIcon(QIcon::fromTheme(procs[i].icon,
                                           QIcon::fromTheme("application-x-executable")));
            name->setData(Qt::UserRole, procs[i].cgroup);
            name->setData(APP_NAME_ROLE, procs[i].name);
            name->setData(PAUSED_ROLE, frozen);
            m_table->setItem(i, 0, name);

            auto *memory = new QTableWidgetItem(fmtSize(procs[i].memory));
            memory->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            memory->setForeground(QColor(105, 105, 110));
            m_table->setItem(i, 1, memory);
            if (procs[i].cgroup == selected) selectedRow = i;
        }
        if (selectedRow < 0 && !procs.isEmpty()) selectedRow = 0;
        if (selectedRow >= 0) m_table->selectRow(selectedRow);
        updateButtons();
    }

protected:
    void showEvent(QShowEvent *event) override {
        ensureUi();
        sampleApps();
        refresh();
        DMainWindow::showEvent(event);
    }

    void closeEvent(QCloseEvent *event) override {
        qInfo() << tr("Window closing, thawing %1 frozen cgroup(s)").arg(m_frozen.size());
        if (!unfreezeAll()) {
            qWarning() << tr("Close blocked: thaw failed, retrying");
            event->ignore();
            QTimer::singleShot(250, this, [this] { close(); });
            return;
        }
        m_postAction.start();
        DMainWindow::closeEvent(event);
        if (event->isAccepted()) releaseUi();
    }

private:
    QTableWidget *m_table = nullptr;
    DPushButton *m_resumeBtn = nullptr;
    DPushButton *m_killBtn = nullptr;
    QTimer *m_timer;
    QList<Proc> m_apps;
    QSet<QString> m_frozen;
    QSet<QString> m_whitelist;
    QHash<QString, quint64> m_lastPgscan;
    quint64 m_lastUserPgscan = 0;
    bool m_hasLastUserPgscan = false;
    bool m_shutdownRequested = false;
    bool m_lastSamplesInvalid = false;
    QElapsedTimer m_pressureSince;
    QElapsedTimer m_reclaimSeen;
    QElapsedTimer m_postAction;
};

int main(int argc, char *argv[]) {
    if (argc == 2 && QByteArray(argv[1]) == "--self-test") return selfTest() ? 0 : 1;

    const int signalFd = createSignalFd();
    if (signalFd < 0) return 1;
    LiferaftApplication a(argc, argv);
    a.setApplicationName("deepin-liferaft");
    a.loadTranslator();
    a.setApplicationDisplayName(QCoreApplication::translate("main", "Deepin Liferaft"));
    Dtk::Core::DLogManager::registerConsoleAppender();
    Dtk::Core::DLogManager::registerJournalAppender();
    const bool hidden = a.arguments().contains("--hidden");
    const QSet<QString> whitelist = loadWhitelist(whitelistPaths());
    qInfo() << QCoreApplication::translate("main", "Application whitelist: %1 entries")
              .arg(whitelist.size());
    a.setQuitOnLastWindowClosed(!hidden);
    qInfo() << QCoreApplication::translate("main", "Deepin Liferaft started: pid=%1 mode=%2")
              .arg(getpid())
              .arg(hidden ? QStringLiteral("hidden") : QStringLiteral("foreground"));
    ForceQuitWindow w(signalFd, whitelist);
    a.setQuitGuard([&w] { return w.unfreezeAll(); });
    if (!hidden) w.show();
    const int result = a.exec();
    if (signalFd >= 0) ::close(signalFd);
    return result;
}

#include "main.moc"
