# NovaEngine: Adaptive UDP Video Sender

![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-%23008FBA.svg?style=for-the-badge&logo=cmake&logoColor=white)
![OpenCV](https://img.shields.io/badge/opencv-%235C3EE8.svg?style=for-the-badge&logo=opencv&logoColor=white)
![FFmpeg](https://img.shields.io/badge/ffmpeg-%23007800.svg?style=for-the-badge&logo=ffmpeg&logoColor=white)

**NovaEngine** is a high-performance, real-time video transmission engine built in C++. It's designed for ultra-low-latency streaming over unreliable networks by using an adaptive, multi-path UDP transport layer. This repository contains the **sender-side implementation**.

The core mission is to maintain frame integrity and achieve near-zero packet loss without relying on standard protocols like WebRTC or SRT, making it ideal for peer-to-peer applications where a direct, resilient connection is critical.

---

## 🚀 Key Features

- **Real-time H.264 Encoding**: Captures video from a camera using OpenCV and encodes it in real-time with a low-latency-tuned FFmpeg pipeline (`tune=zerolatency`).
- **Slice-Based Packetization**: Encoded frames are sliced into MTU-sized chunks (e.g., 1200 bytes) to prevent IP fragmentation and enable finer-grained error control.
- **Forward Error Correction (FEC)**: Integrates Intel's high-performance **ISA-L** library to apply Reed-Solomon error correction codes, allowing the receiver to reconstruct lost packets.
- **Adaptive Multi-Port UDP Transport**:
  - Transmits data over multiple UDP ports simultaneously.
  - A custom `UDPPortProfiler` constantly probes network paths to measure live RTT and packet loss.
  - The `AdaptiveUDPSender` schedules packets on the best available ports based on these live stats.
- **Configurable Redundancy**: Clones critical data packets across different network paths to maximize delivery probability, while sending parity packets more efficiently.

## 🏛️ Sender Architecture

The sender pipeline is designed for speed and resilience.

```mermaid
graph TD
    A[Camera Device] -->|v4l2| B(OpenCV Capture);
    B -->|BGR Frame| C(FFmpeg SwsContext);
    C -->|YUV420P Frame| D(FFmpeg H.264 Encoder);
    D -->|Encoded NALU| E(Frame Slicer);
    E -->|Data Slices (k)| F(ISA-L RS FEC Encoder);
    F -->|Parity Slices (r)| G[Slice Bundler];
    E --> G;
    G -->|Data & Parity Slices| H(AdaptiveUDPSender);
    subgraph Multi-Path UDP Transport
        H --> I{Port 1};
        H --> J{Port 2};
        H --> K{...};
        H --> L{Port N};
    end
    subgraph Live Path Profiling
        M(UDPPortProfiler) -.->|RTT/Loss Stats| H;
        M <--> I;
        M <--> J;
        M <--> K;
        M <--> L;
    end
```

## 🛠️ Build Instructions

### Dependencies
- A C++20 compatible compiler (GCC, Clang)
- **CMake** (>= 3.10)
- **OpenCV**
- **FFmpeg** (`libavcodec`, `libavformat`, `libavutil`, `libswscale`)
- **Intel ISA-L** (`libisal-dev` or compiled from source)
- **pkg-config**

### Quickstart on Debian/Ubuntu
1.  **Install core packages:**
    ```bash
    sudo apt update
    sudo apt install -y build-essential cmake pkg-config \
      libopencv-dev libavcodec-dev libavformat-dev \
      libavutil-dev libswscale-dev libisal-dev
    ```
    *Note: If `libisal-dev` is not available, [build and install it from source](https://github.com/intel/isa-l).*

2.  **Configure and build the project:**
    ```bash
    cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build cmake-build-release -j$(nproc)
    ```

The executable will be located at `cmake-build-release/bin/camera_ffmpeg`.

## ▶️ How to Run

1.  **Configure Network Parameters**: Before running, you may need to edit the `receiver_ip` and `receiver_ports` variables in `main.cpp`.
2.  **Execute**:
    ```bash
    ./cmake-build-release/bin/camera_ffmpeg
    ```

The sender will start capturing video and streaming it to the configured target. Note that this requires a compatible receiver on the other end to handle the custom protocol, echo probes, and decode the stream.

## 🛣️ Roadmap

This project is the foundation of a robust P2P media engine. The next steps are:

- [ ] **Receiver Implementation**: Build a client that can receive, reassemble, and decode the stream.
  - [ ] Jitter Buffer
  - [ ] FEC Decoding
  - [ ] Probe/Heartbeat Echo Logic
- [ ] **Dynamic FEC**: Adjust `k` and `r` parameters based on real-time packet loss.
- [ ] **Congestion Control**: Implement a simple congestion control algorithm to adapt the encoding bitrate.
- [ ] **Advanced Scheduling**: Explore more advanced scheduling algorithms (e.g., UCB1, weighted round-robin) for port selection.
- [ ] **Configuration File**: Move hardcoded network parameters to a runtime config file.
