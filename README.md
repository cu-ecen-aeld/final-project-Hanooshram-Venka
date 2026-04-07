# AESD Final Project

[Project Overview Link](https://github.com/cu-ecen-aeld/final-project-Hanooshram-Venka/wiki/Project-Overview)

---

## Sprint 1 Release
This repository contains the foundational operating system configuration and the network control architecture for our real-time audio DSP project.

### 1. Buildroot 


### 2. Network Control Pathway (`/socket_server`)
This folder contains `socket_daemon.c`, a custom TCP socket server. When executed on the Raspberry Pi, it detaches from the terminal and runs in the background as a daemon listening on Port 9000. It acts as an interface allowing a host laptop to send string commands over the network. In Sprint 2, this daemon will be responsible for catching user commands and updating the real-time DSP audio parameters.
