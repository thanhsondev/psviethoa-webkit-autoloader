#!/bin/bash
# PSVietHoa AutoLoader Installer - Automated Build & Deploy Script

if [ -z "$1" ]; then
    echo "Usage: ./deploy.sh [PS5_IP]"
    exit 1
fi

PS5_IP="$1"
LOADER_PORT="9021"
ELF="installer.elf"
IMAGE_NAME="psviethoa-webkit-autoloader-sdk"

echo "--- Deploying PSVietHoa AutoLoader Installer to $PS5_IP ---"

# 1. Build/verify the docker image
if [[ "$(docker images -q $IMAGE_NAME 2> /dev/null)" == "" ]]; then
    echo "      Docker image $IMAGE_NAME not found. Building... (this may take a few minutes)"
    docker build -t $IMAGE_NAME -f Dockerfile.sdk .
    if [ $? -ne 0 ]; then
        echo "      !!! Docker image build FAILED!"
        exit 1
    fi
    echo "      Docker image built successfully."
fi

# 2. Build the native ELF via Docker
echo "[1/2] Building native ELF via Docker..."
BUILD_LOG="build.log"
if ! docker run --rm -v "$(pwd)":/src -w /src $IMAGE_NAME make clean all > "$BUILD_LOG" 2>&1; then
    echo "      !!! ELF build FAILED! Log below (also saved to $BUILD_LOG):"
    cat "$BUILD_LOG"
    exit 1
fi
rm -f "$BUILD_LOG"
echo "      ELF build successful."

# 3. Send to PS5
if [ -f "$ELF" ]; then
    echo "[2/2] Sending $ELF to $PS5_IP:$LOADER_PORT via socat..."
    socat -u - TCP:$PS5_IP:$LOADER_PORT < "$ELF"
    if [ $? -eq 0 ]; then
        echo "--- Deployment Complete! ---"
    else
        echo "      !!! Failed to send ELF. Is the loader running on PS5?"
        exit 1
    fi
else
    echo "      !!! $ELF not found!"
    exit 1
fi
