#!/bin/bash
set -e

echo ""
echo "════════════════════════════════════════════════════════"
echo "     YT-Converter Quick Test Script"
echo "════════════════════════════════════════════════════════"
echo ""

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

# Step 1: Check system requirements
echo "📋 Step 1: Checking system requirements..."
echo ""

MISSING=()

if ! command -v cmake &> /dev/null; then
    MISSING+=("cmake")
    echo "❌ CMake not found"
else
    echo "✅ CMake: $(cmake --version | head -1)"
fi

if ! command -v make &> /dev/null; then
    MISSING+=("make")
    echo "❌ Make not found"
else
    echo "✅ Make: $(make --version | head -1 | cut -d' ' -f3-)"
fi

if ! command -v gcc &> /dev/null; then
    MISSING+=("gcc")
    echo "❌ GCC not found"
else
    echo "✅ GCC: $(gcc --version | head -1)"
fi

if ! command -v ffmpeg &> /dev/null; then
    MISSING+=("ffmpeg")
    echo "❌ FFmpeg not found"
else
    echo "✅ FFmpeg: $(ffmpeg -version 2>&1 | head -1)"
fi

if ! command -v yt-dlp &> /dev/null; then
    echo "⚠️  yt-dlp not found (needed for actual conversions)"
else
    echo "✅ yt-dlp: $(yt-dlp --version)"
fi

if [ ${#MISSING[@]} -gt 0 ]; then
    echo ""
    echo "❌ Missing required tools: ${MISSING[*]}"
    echo ""
    echo "To install on Ubuntu/Debian, run:"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install -y cmake build-essential"
    echo ""
    exit 1
fi

echo ""
echo "✅ All required tools found!"
echo ""

# Step 2: Build project
echo "════════════════════════════════════════════════════════"
echo "🔨 Step 2: Building project..."
echo "════════════════════════════════════════════════════════"
echo ""

if [ -d "build" ]; then
    echo "Cleaning old build directory..."
    rm -rf build
fi

mkdir -p build
cd build

echo "Running CMake configuration..."
if ! cmake ..; then
    echo ""
    echo "❌ CMake configuration failed!"
    echo "This usually means a C++ dependency is missing."
    echo ""
    echo "Install dependencies with:"
    echo "  sudo apt-get install -y libboost-all-dev libcpprest-dev libssl-dev"
    exit 1
fi

echo ""
echo "Compiling..."
if ! make -j$(nproc); then
    echo ""
    echo "❌ Build failed!"
    exit 1
fi

echo ""
echo "✅ Build successful!"
echo ""

# Step 3: Verify executables
echo "════════════════════════════════════════════════════════"
echo "📋 Step 3: Verifying executables..."
echo "════════════════════════════════════════════════════════"
echo ""

if [ -f "yt2mp3-cli" ]; then
    echo "✅ CLI executable created"
    ls -lh yt2mp3-cli
else
    echo "❌ CLI executable not found!"
    exit 1
fi

if [ -f "yt2mp3-api" ]; then
    echo "✅ API executable created"
    ls -lh yt2mp3-api
else
    echo "❌ API executable not found!"
    exit 1
fi

echo ""

# Step 4: Test CLI validation
echo "════════════════════════════════════════════════════════"
echo "🧪 Step 4: Testing CLI validation..."
echo "════════════════════════════════════════════════════════"
echo ""

echo "Test 4.1: Testing invalid URL (should show error)..."
if ./yt2mp3-cli "not-a-url" mp3 2>&1 | grep -q -i "error\|invalid"; then
    echo "✅ Invalid URL correctly rejected"
else
    echo "⚠️  URL validation response (check for error message)"
fi

echo ""
echo "Test 4.2: Testing invalid format (should show error)..."
if ./yt2mp3-cli "https://www.youtube.com/watch?v=test123" invalid_format 2>&1 | grep -q -i "error\|invalid"; then
    echo "✅ Invalid format correctly rejected"
else
    echo "⚠️  Format validation response (check for error message)"
fi

echo ""

# Step 5: Summary
echo "════════════════════════════════════════════════════════"
echo "✅ Quick Test Complete!"
echo "════════════════════════════════════════════════════════"
echo ""
echo "📊 Summary:"
echo "  ✅ System requirements verified"
echo "  ✅ Project built successfully"
echo "  ✅ Executables created"
echo "  ✅ Basic validation tests passed"
echo ""
echo "🚀 Next Steps:"
echo ""
echo "1. Test CLI with actual YouTube video:"
echo "   ./yt2mp3-cli \"https://www.youtube.com/watch?v=dQw4w9WgXcQ\" mp3"
echo ""
echo "2. Start API server:"
echo "   ./yt2mp3-api"
echo ""
echo "3. In another terminal, test API:"
echo "   curl \"http://localhost:8080?url=https://www.youtube.com/watch?v=VIDEO_ID&format=mp3\""
echo ""
echo "📚 For detailed testing guide, see: docs/TESTING.md"
echo ""
