QT += widgets
CONFIG += c++17
TEMPLATE = app
TARGET = BiggerTask

SOURCES += BiggerTask.cpp

RESOURCES += resources.qrc

# Link against X11 and extensions
LIBS += -lX11 -lXi -lXtst -lXext -lXfixes -lXrandr

# Install rules for Flatpak
target.path = /app/bin
INSTALLS += target

desktop.path = /app/share/applications
desktop.files = flatpak/io.github.taboulet.BiggerTask.desktop
INSTALLS += desktop

icon.path = /app/share/icons/hicolor/scalable/apps
icon.files = flatpak/io.github.taboulet.BiggerTask.svg
INSTALLS += icon

metainfo.path = /app/share/metainfo
metainfo.files = flatpak/io.github.taboulet.BiggerTask.metainfo.xml
INSTALLS += metainfo
