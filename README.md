Note : I decided to archive this project because i kinda lost motivation in fixing the issues it has. As much as it is a really good app for X11 devices, it stills lack wayland support which is really hard for me to do (i know NOTHING about wayland, beside the fact i should use libei) and mostly cuz [Crossmacro](https://github.com/alper-han/CrossMacro) exactly does that and is way more stable, i recommend using it instead if you are using Wayland. I might come back eventually and fix this but not now.

# BiggerTask

A TinyTask clone for Linux

This app is basically a Linux Clone of the Windows app "Tinytask", it's written in c++ and is a simple executable allowing you to :
Record events (Mouse movement/clicks, keyboard inputs)
Play them
Save them for later
Load them to reuse them

You can also choose how much long you want them to loop and even make them loop infinitly

To Stop it, click on the "Ctrl" key
THIS WON'T WORK ON WAYLAND (Cuz of compatibility issues)

<img width="497" height="210" alt="image" src="https://github.com/user-attachments/assets/41756bde-6709-44d8-b70a-8a370f2236e6" />


# Installing

# 1. Flathub (easiest)
[![Download on Flathub](https://flathub.org/assets/badges/flathub-badge-en.svg)](https://flathub.org/apps/io.github.taboulet.BiggerTask)


# 2. Executable
Get the executable in the release tab and run it

# 3. Build from source
Download the zip, extract it, delete the already built executable, open the terminal in the folder and run :
```bash
qmake6 BiggerTask.pro
make
``` 
Make sure you have the required dependencies :

Debian
```bash
sudo apt install build-essential qtbase5-dev qtchooser qt5-qmake qtbase5-dev-tools \
                 libx11-dev libxi-dev libxtst-dev libxext-dev libxfixes-dev libxrandr-dev
```
Fedora
```bash

sudo dnf install @development-tools qt5-qtbase-devel \
                 libX11-devel libXi-devel libXtst-devel libXext-devel libXfixes-devel libXrandr-devel
```
Arch
```bash
sudo pacman -S base-devel qt5-base \
             libx11 libxi libxtst libxext libxfixes libxrandr

``` 



