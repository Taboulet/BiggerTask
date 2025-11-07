QT += widgets
CONFIG += c++17
TEMPLATE = app
TARGET = BiggerTask

SOURCES += BiggerTask.cpp

RESOURCES += resources.qrc

# Link against X11 and extensions
LIBS += -lX11 -lXi -lXtst -lXext -lXfixes -lXrandr
