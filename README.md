# AESD Final Project

[Project Overview Link](https://github.com/cu-ecen-aeld/final-project-Hanooshram-Venka/wiki/Project-Overview)

---

## Sprint 1 Release
This repository contains the foundational operating system configuration and the network control architecture for our real-time audio DSP project.

### 1. Buildroot OS Recipes (`/buildroot_configs`)
This folder contains the "recipes" (`buildroot.config` and `linux_kernel.config`). By dropping these files into a clean Buildroot clone and running `make`, any user can recreate the custom Linux OS with the required USB Audio ALSA drivers hard-baked into the kernel. I have only added these config files as of now instead of pulling in my entire buildoot contents.

### 2. Network Control Pathway (`/socket_server`)
This folder contains `socket_daemon.c`, a custom TCP socket server. When executed on the Raspberry Pi, it detaches from the terminal and runs in the background as a daemon listening on Port 9000. It acts as an interface allowing a host laptop to send string commands over the network. In Sprint 2, this daemon will be responsible for catching user commands and updating the real-time DSP audio parameters.
