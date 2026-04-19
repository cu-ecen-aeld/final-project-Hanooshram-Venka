# AESD Final Project

[Project Overview Link](https://github.com/cu-ecen-aeld/final-project-Hanooshram-Venka/wiki/Project-Overview)

---

## Sprint 1 Release
This repository contains the foundational operating system configuration and the network control architecture for our real-time audio DSP project.

### 1. Buildroot Architecture
This repository uses a Git Submodule linked to the official Buildroot repository. The base_external project tree contains our custom `aesd_final_project_defconfig`, which hard-codes the necessary ALSA and USB audio drivers into the Linux kernel. 


### 2. Network Control Pathway (`/socket_server`)
This folder contains `socket_daemon.c`, a custom TCP socket server. When executed on the Raspberry Pi, it detaches from the terminal and runs in the background as a daemon listening on Port 9000. It acts as an interface allowing a host laptop to send string commands over the network. In Sprint 2, this daemon will be responsible for catching user commands and updating the real-time DSP audio parameters.


## Sprint 2 Release

This release focuses on architectural integration and multithreading. The standalone audio parsing, hardware playback, and networking components from Sprint 1 have been consolidated into a unified, asynchronous application.

### 3. Unified Audio Engine (`/dsp_app`)
This new directory contains the `main.c` file, which transitions the project from synchronous, blocking operations to a **Producer-Consumer** multithreaded architecture using POSIX threads (`pthreads`).

* **Producer Thread (File I/O):** Sequentially reads chunked PCM data from the WAV file using `libsndfile` and pushes it into memory.
* **Consumer Thread (Hardware Playback):** Pulls audio frames from memory and feeds it continuously to the ALSA hardware driver, recovering gracefully from any hardware underruns.
* **Thread-Safe Circular Buffer:** A custom, mutex-protected ring buffer sits between the Producer and Consumer threads. This shared memory queue ensures that the audio hardware receives a smooth, uninterrupted stream of data, thereby protecting playback from irregular read speeds of the file system.
* **Control Thread (Socket IPC):** The TCP socket server from Sprint 1 has been integrated directly into the application as a background control thread. It listens persistently on Port 9000 for network commands and safely updates a globally shared configuration struct using mutex locks. This establishes the IPC (Inter-process communication) pipeline required for real-time parameter manipulation in the next development phase.

Developer Note: The network control thread and the underlying IPC synchronization within main.c were implemented as my primary contribution for Sprint 2 to resolve Issue #6
