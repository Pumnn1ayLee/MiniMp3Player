# MiniMp3Player 🎵

![C++](https://img.shields.io/badge/C++-17-blue.svg)
![Build](https://img.shields.io/badge/Build-CMake-success.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)

MiniMp3Player is a minimalist, high-performance local audio player built with C++17. Underneath its simple ImGui-based user interface lies a hardcore, low-latency audio engine designed with professional audio software standards.

This project was built to explore advanced C++ concurrency, memory management, and Digital Signal Processing (DSP) techniques.

*(📸 Insert a screenshot of your player here: `![Screenshot](docs/screenshot.png)`)*

## ✨ Features

*   **Minimalist UI:** Built with Dear ImGui (DirectX 11 backend) for a lightweight, modern dark-mode aesthetic.
*   **Drag & Drop:** Seamlessly drag and drop `.mp3`, `.wav`, or `.flac` files directly into the window to play.
*   **Real-time Waveform:** Dynamic, buttery-smooth audio waveform visualization synchronized with playback.
*   **Precision Control:** Accurate seeking and smooth volume adjustment.

## 🚀 Core Technical Highlights

This project implements several advanced software engineering paradigms to ensure zero audio dropouts (glitches) and minimal CPU overhead:

### 1. Lock-Free Architecture (无锁架构)

Audio hardware callbacks run at the highest real-time priority in the OS. To prevent **Priority Inversion** and audio glitches, this project implements a **Single-Producer Single-Consumer (SPSC) Lock-Free Ring Buffer**.

*   **O(1) Hardware Sync:** The audio callback thread pulls data strictly in $O(1)$ time without ever acquiring a `std::mutex`.
*   **Cache-Line Alignment:** Atomic read/write pointers are aligned to 64 bytes (`alignas(64)`) to eliminate multi-core False Sharing.

### 2. Zero-Allocation Hot Path (热路径零分配)

Frequent memory allocation during real-time playback causes memory fragmentation and unpredictable GC pauses.

*   Buffers are pre-allocated outside the main decoding loop.
*   During playback, the audio engine reuses these contiguous memory blocks (`std::vector::data()`), reducing runtime `new/delete` operations to absolute zero, keeping CPU usage flat.

### 3. DSP & Audio Quality Optimization (DSP与音质调优)

*   **Audio Dithering (抖动抗混叠):** Configured FFmpeg's `SwrContext` with Triangular Dithering (`SWR_DITHER_TRIANGULAR`) to eliminate 16-bit quantization noise during resampling, preserving high-fidelity audio tails (e.g., piano fades).
*   **Human-Perceptive Waveform rendering:** Instead of naive downsampling, the engine calculates the **Root Mean Square (RMS)** of the audio envelope, converts it to **Decibels (dB)**, and applies **Exponential Moving Average (EMA)** smoothing. This creates a visually stunning, physically accurate waveform that responds naturally to heavy bass and soft melodies.
*   **LTO (Link-Time Optimization):** MSVC compiler flags (`/GL`, `/LTCG`, `/OPT:REF`) are heavily utilized to strip dead code from static libraries (FFmpeg/ImGui), minimizing the final executable size.

## 🛠️ Tech Stack

*   **Language:** C++17
*   **Audio Decoding/Resampling:** [FFmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libswresample)
*   **Audio Hardware I/O:** [miniaudio](https://miniaud.io/) (Single-header library targeting WASAPI/DirectSound)
*   **GUI & Rendering:** [Dear ImGui](https://github.com/ocornut/imgui) + Windows API (Win32) + DirectX 11
*   **Build System:** CMake + [vcpkg](https://github.com/microsoft/vcpkg)

## 📦 Build Instructions

This project uses `CMake` and `vcpkg` for dependency management. Currently optimized for Windows (MSVC).

### Prerequisites

1.  Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with Desktop C++ Workload.
2.  Install [CMake](https://cmake.org/).
3.  Install and bootstrap [vcpkg](https://github.com/microsoft/vcpkg).

### Step 1: Install Dependencies via vcpkg

Open your terminal and run:

```bash
# Install FFmpeg, ImGui (with Win32 & DX11 bindings), and miniaudio
vcpkg install ffmpeg:x64-windows "imgui[win32-binding,dx11-binding]:x64-windows" miniaudio:x64-windows
```

### Step 2: Build the Project

```bash
git clone https://github.com/YourUsername/MiniMp3Player.git
cd MiniMp3Player
mkdir build && cd build

# Replace the path below with your actual vcpkg toolchain path
cmake .. -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Step 3: Run

```bash
./Release/MiniMp3PlayerUI.exe
```

*(Or simply drag and drop an MP3 file onto the executable!)*

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.