<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ko_KR" sourcelanguage="en_US">
<context>
    <name>AppRowDelegate</name>
    <message>
        <location filename="../main.cpp" line="683"/>
        <source>Paused</source>
        <translation>일시 중지됨</translation>
    </message>
</context>
<context>
    <name>ForceQuitWindow</name>
    <message>
        <location filename="../main.cpp" line="872"/>
        <source>Resume</source>
        <translation>재개</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="874"/>
        <source>Force Quit</source>
        <translation>강제 종료</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="811"/>
        <source>Not enough memory</source>
        <translation>메모리가 부족합니다</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="812"/>
        <source>To keep the desktop responsive, applications using the most memory were paused. Resume the ones you still need, or force quit them.</source>
        <translation>데스크톱 응답성을 유지하기 위해 메모리를 많이 사용하는 애플리케이션이 일시 중지되었습니다. 필요한 애플리케이션을 다시 시작하거나 더 이상 필요 없는 애플리케이션을 강제 종료하세요.</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="836"/>
        <source>Application</source>
        <translation>애플리케이션</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="837"/>
        <source>Memory</source>
        <translation>메모리 사용량</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="854"/>
        <source>No applications to show</source>
        <translation>표시할 애플리케이션이 없습니다</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="890"/>
        <source>Resumed %1</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="892"/>
        <source>Resume failed for %1</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="906"/>
        <source>Force quit %1: kill=%2 thaw=%3</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="908"/>
        <source>Force quit %1 failed: kill=%2 thaw=%3</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="978"/>
        <source>Memory pressure above limit: %1%</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="982"/>
        <source>Memory pressure back below limit: %1%</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1002"/>
        <source>Invalid samples: pressure=%1 memory=%2 userPgscan=%3 appSample=%4</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1017"/>
        <source>Trigger %1: pressure=%2%% duration=%3ms recentReclaim=%4 memUsed=%5%% swapUsed=%6%% apps=%7</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1065"/>
        <source>Frozen %1 (%2) trigger=%3 reclaim=%4MB swap=%5MB memory=%6MB</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1075"/>
        <source>Cannot freeze %1 (%2): unreadable or frozen by another component</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1100"/>
        <source>Shutdown requested, thawing %1 frozen cgroup(s)</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1107"/>
        <source>All frozen cgroups thawed, exiting</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1111"/>
        <source>Thaw incomplete (%1 cgroup(s) remain), retrying</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1198"/>
        <source>Window closing, thawing %1 frozen cgroup(s)</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1200"/>
        <source>Close blocked: thaw failed, retrying</source>
        <translation></translation>
    </message>
    <message numerus="yes">
        <location filename="../main.cpp" line="1229"/>
        <source>%n application(s) are still paused</source>
        <translation>
            <numerusform>애플리케이션 %n개가 일시 중지되어 있습니다</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1230"/>
        <source>Closing the window resumes them immediately, and the system may become unresponsive again.</source>
        <translation>창을 닫으면 즉시 다시 시작되며 시스템이 다시 응답하지 않을 수 있습니다.</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1232"/>
        <source>Cancel</source>
        <translation>취소</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1233"/>
        <source>Close and Resume</source>
        <translation>닫고 다시 시작</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1240"/>
        <source>Close cancelled, %1 frozen cgroup(s) stay paused</source>
        <translation>닫기가 취소되었습니다. cgroup %1개는 일시 중지된 상태로 유지됩니다</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1244"/>
        <source>Close confirmed, resuming %1 frozen cgroup(s)</source>
        <translation>닫기가 확인되었습니다. cgroup %1개를 다시 시작합니다</translation>
    </message>
</context>
<context>
    <name>main</name>
    <message>
        <location filename="../main.cpp" line="1280"/>
        <source>Deepin Liferaft</source>
        <translation>Deepin Liferaft</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1284"/>
        <source>Detects sustained memory pressure and pauses the most memory-hungry applications, so you can resume or force quit them before the desktop becomes unusable.</source>
        <translation>메모리 압박이 지속될 때 메모리를 가장 많이 사용하는 애플리케이션을 일시 중지하여 데스크톱이 응답하지 않기 전에 다시 시작하거나 강제 종료할 수 있습니다.</translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1293"/>
        <source>Application whitelist: %1 entries</source>
        <translation></translation>
    </message>
    <message>
        <location filename="../main.cpp" line="1296"/>
        <source>Deepin Liferaft started: pid=%1 mode=%2</source>
        <translation>Deepin Liferaft 시작됨: pid=%1 모드=%2</translation>
    </message>
</context>
</TS>
