// deepin-liferaft: DTK 内存压力对话框：Fedora systemd-oomd 策略触发 →
// 自动弹出，按 DDE 应用 cgroup 内存排序，允许恢复或强制结束应用。
#include <sys/signalfd.h>

#include <DApplication>
#include <DDialog>
#include <DFontSizeManager>
#include <DHorizontalLine>
#include <DLabel>
#include <DListView>
#include <DLog>
#include <DMainWindow>
#include <DPaletteHelper>
#include <DPushButton>
#include <DStandardItem>
#include <DStyle>
#include <DStyledItemDelegate>
#include <DTitlebar>
#include <DWarningButton>

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLocale>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QStackedLayout>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>

#include <signal.h>
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

struct Proc
{
    QString cgroup;
    quint64 memory; // bytes
    quint64 swap;
    std::optional<quint64> pgscan;
    quint64 reclaim = 0;
    QString name;
    QString icon;
};

struct SystemMemory
{
    quint64 memTotal = 0;
    quint64 memAvailable = 0;
    quint64 swapTotal = 0;
    quint64 swapFree = 0;
    bool valid = false;
};

static QString userCgroupPath()
{
    return QString("/sys/fs/cgroup/user.slice/user-%1.slice/user@%1.service").arg(getuid());
}

static double parseFullAvg10(const QByteArray &data)
{
    for (const auto &line : data.split('\n'))
        if (line.startsWith("full"))
            for (const auto &field : line.split(' '))
                if (field.startsWith("avg10="))
                    return field.mid(6).toDouble();
    return -1;
}

static double psiFullAvg10()
{
    QFile f(userCgroupPath() + "/memory.pressure");
    return f.open(QIODevice::ReadOnly) ? parseFullAvg10(f.readAll()) : -1;
}

static std::optional<quint64> fileValue(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    bool ok;
    const quint64 value = f.readAll().trimmed().toULongLong(&ok);
    return ok ? std::optional<quint64>(value) : std::nullopt;
}

static std::optional<quint64> memoryStatValue(const QString &cgroup, const QByteArray &key)
{
    QFile f(cgroup + "/memory.stat");
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    for (const auto &line : f.readAll().split('\n')) {
        if (!line.startsWith(key + ' '))
            continue;
        bool ok;
        const quint64 value = line.mid(key.size() + 1).toULongLong(&ok);
        return ok ? std::optional<quint64>(value) : std::nullopt;
    }
    return std::nullopt;
}

static SystemMemory systemMemory()
{
    SystemMemory out;
    unsigned seen = 0;
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly))
        return out;
    for (const auto &line : f.readAll().split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2)
            continue;
        bool ok;
        const quint64 value = fields[1].toULongLong(&ok);
        if (!ok || value > std::numeric_limits<quint64>::max() / 1024)
            continue;
        const quint64 bytes = value * 1024;
        if (line.startsWith("MemTotal:")) {
            out.memTotal = bytes;
            seen |= 1;
        } else if (line.startsWith("MemAvailable:")) {
            out.memAvailable = bytes;
            seen |= 2;
        } else if (line.startsWith("SwapTotal:")) {
            out.swapTotal = bytes;
            seen |= 4;
        } else if (line.startsWith("SwapFree:")) {
            out.swapFree = bytes;
            seen |= 8;
        }
    }
    out.valid = seen == 15 && out.memAvailable <= out.memTotal && out.swapFree <= out.swapTotal;
    return out;
}

static bool percentAbove(quint64 used, quint64 total, int limit)
{
    return total > 0 && used * 100 > total * quint64(limit);
}

static bool systemSwapPressure(const SystemMemory &memory)
{
    if (!memory.valid)
        return false;
    return percentAbove(memory.memTotal - memory.memAvailable, memory.memTotal, SWAP_USED_LIMIT)
            && percentAbove(memory.swapTotal - memory.swapFree, memory.swapTotal, SWAP_USED_LIMIT);
}

static QString unescapeUnit(QString text)
{
    for (int i = 0; i + 3 < text.size(); ++i) {
        if (text[i] != '\\' || text[i + 1] != 'x')
            continue;
        bool ok;
        const ushort value = text.mid(i + 2, 2).toUShort(&ok, 16);
        if (ok)
            text.replace(i, 4, QChar(value));
    }
    return text;
}

static std::optional<QString> appIdFromUnit(const QString &unit)
{
    QString appId;
    if (unit.startsWith("app-DDE-")) {
        appId = unit.mid(8).section('@', 0, 0);
    } else if (unit.startsWith("app-") && unit.endsWith(".scope")) {
        appId = unit.mid(4, unit.size() - 10);
        const int separator = appId.lastIndexOf('-');
        bool pidOk = false;
        appId.mid(separator + 1).toUInt(&pidOk);
        if (separator <= 0 || !pidOk)
            return std::nullopt;
        appId.truncate(separator);
    } else {
        return std::nullopt;
    }
    appId = unescapeUnit(appId);
    if (appId.endsWith(".autostart"))
        appId.chop(10);
    return appId;
}

static QStringList desktopInfo(const QString &appId)
{
    static QHash<QString, QStringList> cache;
    if (cache.contains(appId))
        return cache.value(appId);

    QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + ".desktop");
    if (file.isEmpty()) {
        const QString linglong =
                "/var/lib/linglong/entries/apps/share/applications/" + appId + ".desktop";
        if (QFile::exists(linglong))
            file = linglong;
    }
    if (file.isEmpty())
        return cache.insert(appId, {}).value();
    QSettings desktop(file, QSettings::IniFormat);
    desktop.beginGroup("Desktop Entry");
    const QString locale = QLocale::system().name();
    QString name = desktop.value("Name[" + locale + "]").toString();
    if (name.isEmpty())
        name = desktop.value("Name[" + locale.section('_', 0, 0) + "]").toString();
    if (name.isEmpty())
        name = desktop.value("Name").toString();
    return cache.insert(appId, { name, desktop.value("Icon").toString() }).value();
}

// Whitelist entries are desktop file IDs (the same ID parsed from the cgroup
// unit name), one per line; blank lines and '#' comments are ignored.
static QSet<QString> parseWhitelist(const QByteArray &data)
{
    QSet<QString> out;
    for (const auto &raw : data.split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        out.insert(QString::fromUtf8(line));
    }
    return out;
}

// vim-style levels, merged as one set: package defaults under
// /usr/share/deepin-liferaft, administrator entries under
// /etc/deepin-liferaft, and per-user entries under ~/.config.
static QStringList whitelistPaths()
{
    QStringList paths;
    for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::AppDataLocation))
        paths << dir + "/whitelist";
    paths << "/etc/deepin-liferaft/whitelist";
    const QString userConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!userConfig.isEmpty())
        paths << userConfig + "/whitelist";
    return paths;
}

static QSet<QString> loadWhitelist(const QStringList &paths)
{
    QSet<QString> out;
    for (const auto &path : paths) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            out.unite(parseWhitelist(file.readAll()));
    }
    return out;
}

static QString ownCgroupPath()
{
    QFile f("/proc/self/cgroup");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    for (const auto &line : f.readAll().split('\n'))
        if (line.startsWith("0::"))
            return "/sys/fs/cgroup" + QString::fromUtf8(line.mid(3)).trimmed();
    return {};
}

static bool writeCgroup(const QString &path, const char *file, const char *value)
{
    QFile control(path + "/" + file);
    return control.open(QIODevice::WriteOnly) && control.write(value) > 0;
}

static bool freezeOwned(const QString &cgroup, QSet<QString> &owned)
{
    const auto state = fileValue(cgroup + "/cgroup.freeze");
    if (!state || *state != 0 || !writeCgroup(cgroup, "cgroup.freeze", "1"))
        return false;
    owned.insert(cgroup);
    return true;
}

static bool thawCgroup(const QString &cgroup)
{
    if (!QFile::exists(cgroup))
        return true;
    const auto state = fileValue(cgroup + "/cgroup.freeze");
    return state && (*state == 0 || writeCgroup(cgroup, "cgroup.freeze", "0"));
}

static bool thawOwned(QSet<QString> &owned)
{
    QSet<QString> failed;
    for (const auto &cgroup : owned)
        if (!thawCgroup(cgroup))
            failed.insert(cgroup);
    owned = failed;
    return owned.isEmpty();
}

static int createSignalFd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
        return -1;
    const int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0)
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    return fd;
}

// DDE 启动组和应用自建 scope 都是独立的应用资源边界。
static std::optional<QList<Proc>>
appProcs(const QString &root, bool requireSwap, bool requirePgscan, const QSet<QString> &whitelist)
{
    QList<Proc> out;
    const QDir appRoot(root);
    if (!appRoot.exists())
        return std::nullopt;
    for (const auto &unit :
         appRoot.entryList({ "app-DDE-*", "app-*.scope" }, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto appId = appIdFromUnit(unit);
        if (!appId || whitelist.contains(*appId))
            continue;
        const QString cgroup = root + "/" + unit;
        const auto memory = fileValue(cgroup + "/memory.current");
        const auto swap = fileValue(cgroup + "/memory.swap.current");
        const auto pgscan = memoryStatValue(cgroup, "pgscan");
        if (!memory || (requireSwap && !swap) || (requirePgscan && !pgscan))
            return std::nullopt;
        if (!*memory)
            continue;

        const auto desktop = desktopInfo(*appId);
        QString name = desktop.value(0);
        if (name.isEmpty())
            name = *appId;
        out << Proc{ cgroup, *memory, swap.value_or(0), pgscan, 0, name, desktop.value(1, *appId) };
    }
    return out;
}

static quint64 pgscanDelta(const std::optional<quint64> &previous,
                           const std::optional<quint64> &current)
{
    return previous && current && *current >= *previous ? *current - *previous : 0;
}

static bool hasSwapCandidate(const QList<Proc> &apps, quint64 swapTotal)
{
    if (!swapTotal)
        return false;
    const quint64 threshold = swapTotal * SWAP_CANDIDATE_LIMIT / 100;
    return std::any_of(apps.cbegin(), apps.cend(), [threshold](const Proc &app) {
        return app.swap > threshold;
    });
}

static Trigger selectTrigger(double pressure,
                             qint64 pressureDuration,
                             bool recentReclaim,
                             const SystemMemory &memory,
                             const QList<Proc> &apps)
{
    if (systemSwapPressure(memory) && hasSwapCandidate(apps, memory.swapTotal))
        return Trigger::Swap;
    if (pressure > PRESSURE_LIMIT && pressureDuration >= PRESSURE_DURATION_MS && recentReclaim)
        return Trigger::Pressure;
    return Trigger::None;
}

static QString fmtSize(quint64 bytes)
{
    if (bytes >= (1ull << 30))
        return QString::number(bytes / 1073741824.0, 'f', 1) + " GB";
    return QString::number(bytes / 1048576.0, 'f', 1) + " MB";
}

// What the machine has and what is left of it. An invalid sample prints no
// numbers at all: a zero here would read like a real measurement.
static QString memoryLine(const SystemMemory &memory)
{
    if (!memory.valid)
        return QCoreApplication::translate("main", "System memory information unavailable");
    return QCoreApplication::translate("main", "Total %1 · Available %2")
            .arg(fmtSize(memory.memTotal), fmtSize(memory.memAvailable));
}

// Swap counts as well, because the pressure policy looks at it. Board without
// swap says so instead of showing two zeroes.
static QString swapLine(const SystemMemory &memory)
{
    if (!memory.valid)
        return QString();
    if (memory.swapTotal == 0)
        return QCoreApplication::translate("main", "Swap not configured");
    return QCoreApplication::translate("main", "Swap total %1 · Available %2")
            .arg(fmtSize(memory.swapTotal), fmtSize(memory.swapFree));
}

static QString triggerName(Trigger trigger)
{
    switch (trigger) {
    case Trigger::Swap:
        return QStringLiteral("swap");
    case Trigger::Pressure:
        return QStringLiteral("pressure");
    case Trigger::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("unknown");
}

static bool freezerSelfTest()
{
    QTemporaryDir dir;
    const QString app = dir.path() + "/app";
    const QString missing = dir.path() + "/missing";
    if (!dir.isValid() || !QDir().mkpath(app) || !QDir().mkpath(missing))
        return false;
    QFile state(app + "/cgroup.freeze");
    if (!state.open(QIODevice::WriteOnly) || state.write("0") != 1)
        return false;
    state.close();

    QSet<QString> owned;
    if (!freezeOwned(app, owned) || fileValue(app + "/cgroup.freeze") != 1)
        return false;
    if (!thawOwned(owned) || fileValue(app + "/cgroup.freeze") != 0)
        return false;

    if (!state.open(QIODevice::WriteOnly) || state.write("1") != 1)
        return false;
    state.close();
    if (freezeOwned(app, owned) || !owned.isEmpty())
        return false;

    owned.insert(missing);
    return !thawOwned(owned) && owned.contains(missing);
}

static bool whitelistSelfTest()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const QString root = dir.path() + "/app.slice";
    const QString allowed = root + "/app-DDE-allowed@1000.service";
    const QString blocked = root + "/app-DDE-blocked@1001.service";
    if (!QDir().mkpath(allowed) || !QDir().mkpath(blocked))
        return false;
    for (const auto &cgroup : { allowed, blocked }) {
        QFile current(cgroup + "/memory.current");
        if (!current.open(QIODevice::WriteOnly) || current.write("1024") <= 0)
            return false;
    }

    const auto all = appProcs(root, false, false, {});
    if (!all || all->size() != 2)
        return false;
    const auto filtered = appProcs(root, false, false, { "blocked" });
    if (!filtered || filtered->size() != 1 || filtered->first().name != "allowed")
        return false;

    const auto parsed = parseWhitelist("# comment\n\nfoo-bar\n  baz \t\n");
    if (parsed != QSet<QString>({ "foo-bar", "baz" }))
        return false;

    QFile systemFile(dir.path() + "/system.list");
    if (!systemFile.open(QIODevice::WriteOnly) || systemFile.write("alpha\n# note\n") <= 0)
        return false;
    systemFile.close();
    QFile userFile(dir.path() + "/user.list");
    if (!userFile.open(QIODevice::WriteOnly) || userFile.write("beta\nalpha\n") <= 0)
        return false;
    userFile.close();
    const auto merged =
            loadWhitelist({ systemFile.fileName(), dir.path() + "/missing", userFile.fileName() });
    return merged == QSet<QString>({ "alpha", "beta" });
}

// Closing the dialog thaws every cgroup this process froze, so an accidental
// close hands the paused applications straight back to the memory pressure that
// paused them. Ask for confirmation while such cgroups are still owned.
static bool needsCloseConfirmation(int frozenCount, bool confirmed)
{
    return frozenCount > 0 && !confirmed;
}

static bool selfTest()
{
    const SystemMemory highSwap{ 100, 9, 100, 9, true };
    const SystemMemory atLimit{ 100, 10, 100, 10, true };
    const SystemMemory boardWithSwap{ 34359738368ull,
                                      4294967296ull,
                                      8589934592ull,
                                      1073741824ull,
                                      true };
    const SystemMemory boardWithoutSwap{ 34359738368ull, 4294967296ull, 0, 0, true };
    const QList<Proc> swapCandidate{ { {}, 1, 6, 0, 0, {}, {} } };
    const QList<Proc> exactSwapLimit{ { {}, 1, 5, 0, 0, {}, {} } };
    return unescapeUnit("google\\x2dchrome") == "google-chrome"
            && appIdFromUnit("app-DDE-google\\x2dchrome@123.service") == "google-chrome"
            && appIdFromUnit("app-code-113545.scope") == "code" && !appIdFromUnit("app-code.scope")
            && parseFullAvg10("some avg10=99.00 avg60=1.00\nfull avg10=50.25 avg60=2.00\n") == 50.25
            && selectTrigger(50.0, 20000, true, {}, {}) == Trigger::None
            && selectTrigger(50.1, 19999, true, {}, {}) == Trigger::None
            && selectTrigger(50.1, 20000, false, {}, {}) == Trigger::None
            && selectTrigger(50.1, 20000, true, {}, {}) == Trigger::Pressure
            && selectTrigger(0, 0, false, highSwap, swapCandidate) == Trigger::Swap
            && selectTrigger(0, 0, false, atLimit, swapCandidate) == Trigger::None
            && selectTrigger(0, 0, false, highSwap, exactSwapLimit) == Trigger::None
            && selectTrigger(0, 0, false, highSwap, {}) == Trigger::None
            && pgscanDelta(100, 101) == 1 && pgscanDelta(100, std::nullopt) == 0
            && pgscanDelta(std::nullopt, 101) == 0 && freezerSelfTest() && whitelistSelfTest()
            && !needsCloseConfirmation(0, false) && !needsCloseConfirmation(0, true)
            && needsCloseConfirmation(1, false) && needsCloseConfirmation(3, false)
            && !needsCloseConfirmation(1, true) && fmtSize(56727962) == "54.1 MB"
            && fmtSize(1610612736) == "1.5 GB"
            && memoryLine(boardWithSwap) == "Total 32.0 GB · Available 4.0 GB"
            && swapLine(boardWithSwap) == "Swap total 8.0 GB · Available 1.0 GB"
            && swapLine(boardWithoutSwap) == "Swap not configured"
            && memoryLine({}) == "System memory information unavailable" && swapLine({}).isEmpty();
}

static const int APP_NAME_ROLE = Qt::UserRole + 1;
static const int PAUSED_ROLE = Qt::UserRole + 2;
static const int MEMORY_ROLE = Qt::UserRole + 3;
static const int CGROUP_ROLE = Qt::UserRole + 4;

static const int APP_ROW_HEIGHT = 44;
static const int LIST_CONTENT_MARGIN = 12;
static const int MAX_LIST_ROWS = 10;

// Warning badge used by the alert banner: the themed warning icon on a soft
// tint of the warning color, following DDE's alert styling.
class AlertBadge : public QWidget
{
public:
    explicit AlertBadge(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(36, 36);
        m_icon = QIcon::fromTheme(QStringLiteral("dialog-warning"),
                                  style()->standardIcon(QStyle::SP_MessageBoxWarning));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor tint = DPaletteHelper::instance()->palette(this).color(DPalette::TextWarning);
        tint.setAlphaF(0.16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tint);
        painter.drawRoundedRect(rect(), 10, 10);

        QRect iconRect(QPoint(), QSize(22, 22));
        iconRect.moveCenter(rect().center());
        m_icon.paint(&painter, iconRect);
    }

private:
    QIcon m_icon;
};

// Alert header card: a rounded background holding the warning badge and two
// lines of text, in the banner style used across DDE.
class AlertBanner : public QFrame
{
public:
    AlertBanner(const QString &title, const QString &message, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(14);
        layout->addWidget(new AlertBadge(this), 0, Qt::AlignTop);

        auto *textLayout = new QVBoxLayout;
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(3);

        auto *titleLabel = new DLabel(title, this);
        titleLabel->setWordWrap(true);
        titleLabel->setElideMode(Qt::ElideNone);
        DFontSizeManager::instance()->bind(titleLabel, DFontSizeManager::T5, QFont::DemiBold);

        auto *messageLabel = new DLabel(message, this);
        messageLabel->setWordWrap(true);
        messageLabel->setElideMode(Qt::ElideNone);
        messageLabel->setForegroundRole(DPalette::TextTips);
        DFontSizeManager::instance()->bind(messageLabel, DFontSizeManager::T8);

        textLayout->addWidget(titleLabel);
        textLayout->addWidget(messageLabel);
        textLayout->addSpacing(4);

        m_memoryLabel = new DLabel(this);
        m_swapLabel = new DLabel(this);
        for (auto *label : { m_memoryLabel, m_swapLabel }) {
            label->setWordWrap(true);
            label->setElideMode(Qt::ElideNone);
            // Plain text rather than the tips color, so the numbers read as data
            // instead of more prose.
            label->setForegroundRole(DPalette::Text);
            DFontSizeManager::instance()->bind(label, DFontSizeManager::T9);
            textLayout->addWidget(label);
        }
        layout->addLayout(textLayout, 1);
    }

    // Machine memory and swap under the alert, refreshed with every poll. An
    // empty swap line hides that row.
    void setMemoryStatus(const QString &memory, const QString &swap)
    {
        if (m_memoryLabel->text() != memory)
            m_memoryLabel->setText(memory);
        m_swapLabel->setVisible(!swap.isEmpty());
        if (m_swapLabel->text() != swap)
            m_swapLabel->setText(swap);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(
                DPaletteHelper::instance()->palette(this).color(DPalette::ObviousBackground));
        const int radius = DStyleHelper(style()).pixelMetric(DStyle::PM_FrameRadius);
        painter.drawRoundedRect(rect(), radius, radius);
    }

private:
    DLabel *m_memoryLabel = nullptr;
    DLabel *m_swapLabel = nullptr;

};

// Draws one application row — icon, name, paused tag and right-aligned memory
// usage — on an alternating row background. The striping follows
// deepin-system-monitor's BaseTableView::drawRow(): even rows use
// DPalette::AlternateBase, odd rows DPalette::Base, the selected row uses the
// accent color and hovering darkens the base color.
class AppRowDelegate : public DStyledItemDelegate
{
public:
    explicit AppRowDelegate(DListView *view)
        : DStyledItemDelegate(view)
    {
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const DPalette palette = DPaletteHelper::instance()->palette(opt.widget);
        QPalette::ColorGroup group =
                opt.state.testFlag(QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
        if (group == QPalette::Normal && !opt.state.testFlag(QStyle::State_Active))
            group = QPalette::Inactive;

        const QColor baseColor = palette.color(DPalette::Base);
        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        const bool hovered = opt.state.testFlag(QStyle::State_MouseOver);
        QColor background = (index.row() % 2) ? baseColor : palette.color(DPalette::AlternateBase);
        if (selected)
            background = hovered ? DStyle::adjustColor(palette.color(DPalette::Highlight), 0, 0, 20)
                                 : palette.color(DPalette::Highlight);
        else if (hovered)
            background = DStyle::adjustColor(baseColor, 0, 0, -10);

        const QColor textColor =
                opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        const QColor tipsColor = selected ? textColor : palette.color(DPalette::TextTips);
        const QColor warningColor = selected ? textColor : palette.color(DPalette::TextWarning);

        const QRect content = opt.rect.marginsRemoved(margins());
        const QStyle *viewStyle = opt.widget ? opt.widget->style() : QApplication::style();
        const int radius =
                DStyleHelper(viewStyle).pixelMetric(DStyle::PM_FrameRadius, &opt, opt.widget);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        painter->drawRoundedRect(opt.rect, radius, radius);

        const int iconSize = 24;
        const QRect iconRect(content.left(),
                             content.center().y() - iconSize / 2,
                             iconSize,
                             iconSize);
        index.data(Qt::DecorationRole)
                .value<QIcon>()
                .paint(painter,
                       iconRect,
                       Qt::AlignCenter,
                       selected ? QIcon::Selected : QIcon::Normal);

        const QString memory = index.data(MEMORY_ROLE).toString();
        const QFont memoryFont = DFontSizeManager::instance()->get(DFontSizeManager::T8, opt.font);
        const QFontMetricsF memoryMetrics(memoryFont);
        const int memoryWidth = int(memoryMetrics.horizontalAdvance(memory)) + 4;
        const QRect memoryRect(content.right() - memoryWidth,
                               content.top(),
                               memoryWidth,
                               content.height());

        int nameRight = memoryRect.left() - 10;
        const QString tagText = index.data(PAUSED_ROLE).toBool()
                ? QCoreApplication::translate("AppRowDelegate", "Paused")
                : QString();
        if (!tagText.isEmpty()) {
            const QFont tagFont = DFontSizeManager::instance()->get(DFontSizeManager::T9, opt.font);
            const QFontMetricsF tagMetrics(tagFont);
            const int tagWidth = int(tagMetrics.horizontalAdvance(tagText)) + 14;
            const int tagHeight = 18;
            const QRect tagRect(nameRight - tagWidth,
                                content.center().y() - tagHeight / 2,
                                tagWidth,
                                tagHeight);
            QColor tagBackground = warningColor;
            tagBackground.setAlphaF(selected ? 0.22 : 0.16);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tagBackground);
            painter->drawRoundedRect(tagRect, 4, 4);
            painter->setFont(tagFont);
            painter->setPen(warningColor);
            painter->drawText(tagRect, Qt::AlignCenter, tagText);
            nameRight = tagRect.left() - 8;
        }

        QFont nameFont = DFontSizeManager::instance()->get(DFontSizeManager::T7, opt.font);
        nameFont.setWeight(QFont::Medium);
        const QFontMetricsF nameMetrics(nameFont);
        const QRect nameRect(iconRect.right() + 10,
                             content.top(),
                             std::max(0, nameRight - iconRect.right() - 10),
                             content.height());
        painter->setFont(nameFont);
        painter->setPen(textColor);
        painter->drawText(nameRect,
                          Qt::AlignVCenter | Qt::AlignLeft,
                          nameMetrics.elidedText(index.data(APP_NAME_ROLE).toString(),
                                                 Qt::ElideRight,
                                                 nameRect.width()));

        painter->setFont(memoryFont);
        painter->setPen(tipsColor);
        painter->drawText(memoryRect, Qt::AlignVCenter | Qt::AlignRight, memory);
        painter->restore();
    }
};

// Answer to a quit request: Allow lets the application exit, Retry repeats the
// request after a thaw that failed, WaitForUser keeps the application alive
// while the close confirmation waits for an answer.
enum class QuitRequest { Allow, Retry, WaitForUser };

class LiferaftApplication : public DApplication
{
public:
    using DApplication::DApplication;

    void setQuitGuard(std::function<QuitRequest()> guard) { m_quitGuard = std::move(guard); }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Quit && m_quitGuard) {
            const QuitRequest answer = m_quitGuard();
            if (answer != QuitRequest::Allow) {
                if (answer == QuitRequest::Retry && !m_quitRetryScheduled) {
                    m_quitRetryScheduled = true;
                    QTimer::singleShot(250, this, [this] {
                        m_quitRetryScheduled = false;
                        QCoreApplication::quit();
                    });
                }
                return true;
            }
        }
        return DApplication::event(event);
    }

private:
    std::function<QuitRequest()> m_quitGuard;
    bool m_quitRetryScheduled = false;
};

class ForceQuitWindow : public DMainWindow
{
    Q_OBJECT
public:
    explicit ForceQuitWindow(int signalFd, QSet<QString> whitelist)
        : m_whitelist(std::move(whitelist))
    {
        setWindowTitle(QGuiApplication::applicationDisplayName());
        resize(560, 620);
        setMinimumSize(520, 480);

        if (const auto pgscan = memoryStatValue(userCgroupPath(), "pgscan")) {
            m_lastUserPgscan = *pgscan;
            m_hasLastUserPgscan = true;
        }

        if (signalFd >= 0) {
            auto *notifier = new QSocketNotifier(signalFd, QSocketNotifier::Read, this);
            connect(notifier, &QSocketNotifier::activated, this, [this, signalFd] {
                signalfd_siginfo info;
                while (::read(signalFd, &info, sizeof(info)) == sizeof(info)) { }
                requestShutdown();
            });
        }

        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this] {
            poll();
        });
        m_timer->start(POLL_MS);
    }

    void ensureUi()
    {
        if (m_view)
            return;

        const QIcon icon(":/icons/deepin-liferaft.svg");
        setWindowIcon(icon);
        titlebar()->setIcon(icon);
        titlebar()->setTitle(windowTitle());

        auto *central = new QWidget;
        auto *vbox = new QVBoxLayout(central);
        vbox->setContentsMargins(16, 8, 16, 12);
        vbox->setSpacing(12);

        m_banner = new AlertBanner(
                tr("Not enough memory"),
                tr("To keep the desktop responsive, applications using the most memory were "
                   "paused. Resume the ones you still need, or force quit them."),
                central);
        vbox->addWidget(m_banner);

        m_view = new DListView(central);
        m_view->setFrameShape(QFrame::NoFrame);
        m_view->setSelectionMode(QAbstractItemView::SingleSelection);
        m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_view->setUniformItemSizes(true);
        m_view->viewport()->setAttribute(Qt::WA_Hover, true);
        m_view->setItemDelegate(new AppRowDelegate(m_view));
        m_view->setItemSize(QSize(0, APP_ROW_HEIGHT));
        m_view->setItemSpacing(0);
        m_view->setItemMargins(QMargins(LIST_CONTENT_MARGIN, 6, LIST_CONTENT_MARGIN, 6));
        m_model = new QStandardItemModel(m_view);
        m_view->setModel(m_model);

        // Column captions, aligned with the content rect of every row.
        auto *captions = new QWidget(central);
        auto *captionLayout = new QHBoxLayout(captions);
        captionLayout->setContentsMargins(LIST_CONTENT_MARGIN, 0, LIST_CONTENT_MARGIN, 0);
        captionLayout->setSpacing(10);
        auto *nameCaption = new DLabel(tr("Application"), captions);
        auto *memoryCaption = new DLabel(tr("Memory"), captions);
        for (auto *caption : { nameCaption, memoryCaption }) {
            caption->setForegroundRole(DPalette::TextTips);
            DFontSizeManager::instance()->bind(caption, DFontSizeManager::T9);
        }
        captionLayout->addWidget(nameCaption);
        captionLayout->addStretch();
        captionLayout->addWidget(memoryCaption);

        auto *listPage = new QWidget(central);
        auto *listLayout = new QVBoxLayout(listPage);
        listLayout->setContentsMargins(0, 0, 0, 0);
        listLayout->setSpacing(8);
        listLayout->addWidget(captions);
        listLayout->addWidget(new DHorizontalLine(listPage));
        listLayout->addWidget(m_view, 1);

        auto *emptyLabel = new DLabel(tr("No applications to show"), central);
        emptyLabel->setForegroundRole(DPalette::TextTips);
        auto *emptyPage = new QWidget(central);
        auto *emptyLayout = new QVBoxLayout(emptyPage);
        emptyLayout->setContentsMargins(0, 0, 0, 0);
        emptyLayout->addWidget(emptyLabel, 0, Qt::AlignCenter);

        m_listPages = new QStackedLayout;
        m_listPages->addWidget(listPage);
        m_listPages->addWidget(emptyPage);
        auto *listHost = new QWidget(central);
        listHost->setLayout(m_listPages);
        vbox->addWidget(listHost, 1);

        vbox->addWidget(new DHorizontalLine(central));

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        m_resumeBtn = new DPushButton(tr("Resume"), central);
        m_killBtn = new DWarningButton(central);
        m_killBtn->setText(tr("Force Quit"));
        buttons->addStretch();
        buttons->addWidget(m_resumeBtn);
        buttons->addWidget(m_killBtn);
        vbox->addLayout(buttons);
        setCentralWidget(central);

        connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
            updateButtons();
        });
        connect(m_resumeBtn, &DPushButton::clicked, this, [this] {
            const QString cgroup = currentCgroup();
            if (cgroup.isEmpty())
                return;
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
            const QString cgroup = currentCgroup();
            if (cgroup.isEmpty())
                return;
            const bool owned = m_frozen.contains(cgroup);
            const bool killed = writeCgroup(cgroup, "cgroup.kill", "1");
            const bool thawed = !owned || thawCgroup(cgroup);
            if (killed && thawed) {
                m_frozen.remove(cgroup);
                qInfo() << tr("Force quit %1: kill=%2 thaw=%3").arg(cgroup).arg(killed).arg(thawed);
            } else {
                qWarning() << tr("Force quit %1 failed: kill=%2 thaw=%3")
                                      .arg(cgroup)
                                      .arg(killed)
                                      .arg(thawed);
            }
            sampleApps();
            refresh();
        });
    }

    void releaseUi()
    {
        if (!m_view)
            return;
        QWidget *central = takeCentralWidget();
        m_view = nullptr;
        m_model = nullptr;
        m_listPages = nullptr;
        m_banner = nullptr;
        m_resumeBtn = nullptr;
        m_killBtn = nullptr;
        central->deleteLater();
    }

    bool sampleApps(bool requireSwap = false, bool requirePgscan = false)
    {
        const auto snapshot =
                appProcs(userCgroupPath() + "/app.slice", requireSwap, requirePgscan, m_whitelist);
        if (!snapshot) {
            if (requirePgscan)
                m_lastPgscan.clear();
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
            if (app.pgscan)
                current.insert(app.cgroup, *app.pgscan);
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

    void poll()
    {
        bool userPgscanValid = false;
        if (const auto pgscan = memoryStatValue(userCgroupPath(), "pgscan")) {
            if (m_hasLastUserPgscan && *pgscan > m_lastUserPgscan)
                m_reclaimSeen.start();
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
        // The dialog reports what the pressure policy sees, so keep the latest
        // sample for the alert's memory and swap lines.
        m_memory = memory;
        const bool pressureSampling = pressure > PRESSURE_LIMIT;
        const bool swapSampling = systemSwapPressure(memory);
        bool appSampleValid = true;
        if (pressureSampling || swapSampling || isVisible())
            appSampleValid = sampleApps(swapSampling, pressureSampling);

        const bool recentReclaim =
                m_reclaimSeen.isValid() && m_reclaimSeen.elapsed() <= RECLAIM_WINDOW_MS;
        const qint64 pressureDuration = m_pressureSince.isValid() ? m_pressureSince.elapsed() : -1;
        const bool samplesValid =
                pressure >= 0 && memory.valid && userPgscanValid && appSampleValid;
        if (!samplesValid) {
            if (!m_lastSamplesInvalid) {
                m_lastSamplesInvalid = true;
                qWarning()
                        << tr("Invalid samples: pressure=%1 memory=%2 userPgscan=%3 appSample=%4")
                                   .arg(pressure >= 0)
                                   .arg(memory.valid)
                                   .arg(userPgscanValid)
                                   .arg(appSampleValid);
            }
        } else
            m_lastSamplesInvalid = false;
        const Trigger trigger = samplesValid
                ? selectTrigger(pressure, pressureDuration, recentReclaim, memory, m_apps)
                : Trigger::None;
        const bool coolingDown =
                m_postAction.isValid() && m_postAction.elapsed() < POST_ACTION_DELAY_MS;

        if (trigger != Trigger::None && !coolingDown && !isVisible()) {
            qInfo() << tr("Trigger %1: pressure=%2%% duration=%3ms recentReclaim=%4 "
                          "memUsed=%5%% swapUsed=%6%% apps=%7")
                               .arg(triggerName(trigger))
                               .arg(pressure, 0, 'f', 1)
                               .arg(pressureDuration)
                               .arg(recentReclaim)
                               .arg(memory.memTotal ? (memory.memTotal - memory.memAvailable) * 100
                                                    / memory.memTotal
                                                    : 0)
                               .arg(memory.swapTotal ? (memory.swapTotal - memory.swapFree) * 100
                                                    / memory.swapTotal
                                                     : 0)
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

    void freezeCandidates(Trigger trigger, quint64 swapTotal)
    {
        auto candidates = m_apps;
        if (trigger == Trigger::Swap)
            std::sort(candidates.begin(), candidates.end(), [](const Proc &a, const Proc &b) {
                return a.swap > b.swap;
            });
        else
            std::sort(candidates.begin(), candidates.end(), [](const Proc &a, const Proc &b) {
                return a.reclaim == b.reclaim ? a.memory > b.memory : a.reclaim > b.reclaim;
            });

        const quint64 swapThreshold = swapTotal * SWAP_CANDIDATE_LIMIT / 100;
        const QString ownCgroup = ownCgroupPath();
        if (ownCgroup.isEmpty())
            return;
        int frozen = 0;
        for (const auto &app : candidates) {
            if (trigger == Trigger::Swap && app.swap <= swapThreshold)
                continue;
            if (ownCgroup == app.cgroup || ownCgroup.startsWith(app.cgroup + '/'))
                continue;
            if (m_frozen.contains(app.cgroup))
                continue;
            if (freezeOwned(app.cgroup, m_frozen)) {
                qInfo() << tr("Frozen %1 (%2) trigger=%3 reclaim=%4MB swap=%5MB memory=%6MB")
                                   .arg(app.name)
                                   .arg(app.cgroup)
                                   .arg(triggerName(trigger))
                                   .arg(app.reclaim / 1048576.0, 0, 'f', 1)
                                   .arg(app.swap / 1048576.0, 0, 'f', 1)
                                   .arg(app.memory / 1048576.0, 0, 'f', 1);
                if (++frozen == 3)
                    break;
            } else {
                qWarning() << tr("Cannot freeze %1 (%2): unreadable or frozen by another component")
                                      .arg(app.name)
                                      .arg(app.cgroup);
            }
        }
        refresh();
    }

    bool unfreezeAll() { return thawOwned(m_frozen); }

    // A quit request from the titlebar menu or from any other quit() call takes
    // the same confirmation as closing the window while cgroups are paused.
    QuitRequest handleQuitRequest()
    {
        if (!m_shutdownRequested && isVisible() && !confirmClose())
            return QuitRequest::WaitForUser;
        m_shutdownRequested = true;
        return unfreezeAll() ? QuitRequest::Allow : QuitRequest::Retry;
    }

    void requestShutdown()
    {
        if (m_shutdownRequested)
            return;
        m_shutdownRequested = true;
        qInfo() << tr("Shutdown requested, thawing %1 frozen cgroup(s)").arg(m_frozen.size());
        retryShutdown();
    }

    void retryShutdown()
    {
        if (unfreezeAll()) {
            qInfo() << tr("All frozen cgroups thawed, exiting");
            qApp->quit();
        } else {
            qWarning()
                    << tr("Thaw incomplete (%1 cgroup(s) remain), retrying").arg(m_frozen.size());
            QTimer::singleShot(250, this, [this] {
                retryShutdown();
            });
        }
    }

    void updateButtons()
    {
        if (!m_view)
            return;
        const QString cgroup = currentCgroup();
        m_resumeBtn->setEnabled(!cgroup.isEmpty() && m_frozen.contains(cgroup));
        m_killBtn->setEnabled(!cgroup.isEmpty());
    }

    QString currentCgroup() const
    {
        const QModelIndex index = m_view ? m_view->currentIndex() : QModelIndex();
        return index.isValid() ? index.data(CGROUP_ROLE).toString() : QString();
    }

    void refreshStatus()
    {
        if (m_banner)
            m_banner->setMemoryStatus(memoryLine(m_memory), swapLine(m_memory));
    }


    void refresh()
    {
        if (!m_view || !m_model)
            return;
        refreshStatus();
        const QString selected = currentCgroup();

        auto procs = m_apps;
        std::sort(procs.begin(), procs.end(), [this](const Proc &a, const Proc &b) {
            const bool aFrozen = m_frozen.contains(a.cgroup);
            const bool bFrozen = m_frozen.contains(b.cgroup);
            return aFrozen == bFrozen ? a.memory > b.memory : aFrozen;
        });
        if (procs.size() > MAX_LIST_ROWS)
            procs.resize(MAX_LIST_ROWS);

        // Update rows in place so scrolling, hovering and selection survive the
        // one-second refresh while the dialog is open.
        while (m_model->rowCount() > procs.size())
            m_model->removeRow(m_model->rowCount() - 1);
        while (m_model->rowCount() < procs.size())
            m_model->appendRow(new DStandardItem);

        int selectedRow = -1;
        for (int i = 0; i < procs.size(); ++i) {
            const bool frozen = m_frozen.contains(procs[i].cgroup);
            auto *item = static_cast<DStandardItem *>(m_model->item(i));
            item->setData(procs[i].cgroup, CGROUP_ROLE);
            item->setData(procs[i].name, APP_NAME_ROLE);
            item->setData(frozen, PAUSED_ROLE);
            item->setData(fmtSize(procs[i].memory), MEMORY_ROLE);
            item->setIcon(
                    QIcon::fromTheme(procs[i].icon, QIcon::fromTheme("application-x-executable")));
            if (procs[i].cgroup == selected)
                selectedRow = i;
        }

        if (selectedRow < 0 && !procs.isEmpty())
            selectedRow = 0;
        if (selectedRow >= 0)
            m_view->setCurrentIndex(m_model->index(selectedRow, 0));
        m_listPages->setCurrentIndex(procs.isEmpty() ? 1 : 0);
        updateButtons();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        ensureUi();
        // A window that appears again belongs to a new pressure episode: ask
        // again before this one closes with applications still paused.
        m_closeConfirmed = false;
        // Read the machine state once for the first paint; the poll keeps it up
        // to date from then on.
        m_memory = systemMemory();
        sampleApps();
        refresh();
        DMainWindow::showEvent(event);
    }

    void closeEvent(QCloseEvent *event) override
    {
        // A shutdown from SIGTERM or SIGINT resumes the paused applications
        // without asking; there is no user in front of a service stop.
        if (!m_shutdownRequested && !confirmClose()) {
            event->ignore();
            return;
        }

        qInfo() << tr("Window closing, thawing %1 frozen cgroup(s)").arg(m_frozen.size());
        if (!unfreezeAll()) {
            qWarning() << tr("Close blocked: thaw failed, retrying");
            event->ignore();
            QTimer::singleShot(250, this, [this] {
                close();
            });
            return;
        }
        m_postAction.start();
        DMainWindow::closeEvent(event);
        if (event->isAccepted())
            releaseUi();
    }

private:
    // Closing resumes every application this process paused, so a close that
    // was not meant as such must be confirmed first. Returns true when the
    // close may proceed.
    bool confirmClose()
    {
        if (!needsCloseConfirmation(m_frozen.size(), m_closeConfirmed))
            return true;
        if (m_confirmingClose) {
            // A prompt is already on screen; keep the window until it is answered.
            return false;
        }

        DDialog prompt(this);
        prompt.setTitle(tr("%n application(s) are still paused", nullptr, m_frozen.size()));
        prompt.setMessage(tr("Closing the window resumes them immediately, and the system may "
                             "become unresponsive again."));
        prompt.addButton(tr("Cancel"), true, DDialog::ButtonNormal);
        prompt.addButton(tr("Close and Resume"), false, DDialog::ButtonWarning);

        m_confirmingClose = true;
        const int choice = prompt.exec();
        m_confirmingClose = false;

        if (choice != 1) {
            qInfo() << tr("Close cancelled, %1 frozen cgroup(s) stay paused").arg(m_frozen.size());
            return false;
        }
        m_closeConfirmed = true;
        qInfo() << tr("Close confirmed, resuming %1 frozen cgroup(s)").arg(m_frozen.size());
        return true;
    }

    DListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    QStackedLayout *m_listPages = nullptr;
    AlertBanner *m_banner = nullptr;
    DPushButton *m_resumeBtn = nullptr;
    DPushButton *m_killBtn = nullptr;
    QTimer *m_timer;
    QList<Proc> m_apps;
    QSet<QString> m_frozen;
    QSet<QString> m_whitelist;
    QHash<QString, quint64> m_lastPgscan;
    quint64 m_lastUserPgscan = 0;
    SystemMemory m_memory;
    bool m_hasLastUserPgscan = false;
    bool m_shutdownRequested = false;
    bool m_confirmingClose = false;
    bool m_closeConfirmed = false;
    bool m_lastSamplesInvalid = false;
    QElapsedTimer m_pressureSince;
    QElapsedTimer m_reclaimSeen;
    QElapsedTimer m_postAction;
};

int main(int argc, char *argv[])
{
    if (argc == 2 && QByteArray(argv[1]) == "--self-test")
        return selfTest() ? 0 : 1;

    const int signalFd = createSignalFd();
    if (signalFd < 0)
        return 1;
    LiferaftApplication a(argc, argv);
    a.setApplicationName("deepin-liferaft");
    a.loadTranslator();
    a.setApplicationDisplayName(QCoreApplication::translate("main", "Deepin Liferaft"));
    a.setProductIcon(QIcon(QStringLiteral(":/icons/deepin-liferaft.svg")));
    a.setApplicationVersion(QStringLiteral(LIFERAFT_VERSION));
    a.setApplicationDescription(
            QCoreApplication::translate("main",
                                        "Detects sustained memory pressure and pauses the "
                                        "most memory-hungry applications, so you can resume "
                                        "or force quit them before the desktop becomes unusable."));
    a.setApplicationHomePage(QStringLiteral("https://github.com/st0nie/deepin-liferaft"));
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
    a.setQuitGuard([&w] {
        return w.handleQuitRequest();
    });
    if (!hidden)
        w.show();
    const int result = a.exec();
    if (signalFd >= 0)
        ::close(signalFd);
    return result;
}

#include "main.moc"
