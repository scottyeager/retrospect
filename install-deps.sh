#!/bin/bash
# Install build dependencies for Retrospect on Debian/Ubuntu
set -e

apt-get update
apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libncurses-dev \
    liblo-dev \
    libjack-jackd2-dev \
    libasound2-dev \
    libfreetype-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxcomposite-dev
