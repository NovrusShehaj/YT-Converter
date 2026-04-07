# YT-Converter Complete Testing Guide

## System Requirements Check

### Currently Available Tools
- ✅ GCC/G++ Compiler
- ✅ Make Build Tool  
- ✅ FFmpeg
- ❌ CMake (NOT FOUND - needed for building)
- ❌ yt-dlp (NOT FOUND - needed for downloading videos)
- ❌ cpprestsdk (NOT FOUND - needed for REST API)
- ❌ Boost Libraries (NOT FOUND - needed for utilities)
- ❌ OpenSSL (NOT FOUND - needed for security)

## Step 1: Install Missing Dependencies

### Ubuntu/Debian System
```bash
# Update package manager
sudo apt-get update

# Install build essentials
sudo apt-get install -y cmake build-essential

# Install C++ libraries
sudo apt-get install -y \
  libboost-all-dev \
  libcpprest-dev \
  libssl-dev

# Install FFmpeg (already installed)
sudo apt-get install -y ffmpeg

# Install yt-dlp for video downloading
sudo apt-get install -y yt-dlp
# OR using pip
pip install --upgrade yt-dlp
```

## Step 2: Build the Project

```bash
cd /home/ghost/Programming/YT-Converter

# Clean build directory
rm -rf build

# Create new build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Compile (use all CPU cores)
make -j$(nproc)

# Verify builds succeeded
echo "Build status: $?"
```

## Step 3: Verify Executables Created

```bash
# Check if executables exist
ls -lh yt2mp3-cli yt2mp3-api
./yt2mp3-cli --version 2>/dev/null || echo "CLI ready"
./yt2mp3-api --version 2>/dev/null || echo "API ready"
```

## Step 4: Test CLI (Command Line Interface)

### Test 4.1: Display Help
```bash
cd /home/ghost/Programming/YT-Converter/build
./yt2mp3-cli --help
```

### Test 4.2: Test URL Validation (should fail gracefully)
```bash
./yt2mp3-cli "https://invalid-url.com" mp3
# Expected: Error about invalid YouTube URL
```

### Test 4.3: Test Format Validation (should fail gracefully)
```bash
./yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" invalid
# Expected: Error about invalid format
```

### Test 4.4: Actual Conversion (requires working yt-dlp)
```bash
# Convert YouTube video to MP3
./yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" mp3

# Check output file
ls -lh ../MP3/
```

### Test 4.5: Test All Formats
```bash
# Test MP3
./yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" mp3
ls -lh ../MP3/

# Test MP4
./yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" mp4
ls -lh ../MP4/

# Test WAV
./yt2mp3-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" wav
ls -lh ../WAV/
```

## Step 5: Test REST API Server

### Test 5.1: Start API Server (in background)
```bash
cd /home/ghost/Programming/YT-Converter/build
./yt2mp3-api > api.log 2>&1 &
API_PID=$!
echo "API started with PID: $API_PID"
sleep 2  # Wait for server to start

# Verify server is running
ps -p $API_PID
```

### Test 5.2: Test API Endpoints with curl

#### Test missing parameters (should return 400 error)
```bash
curl "http://localhost:8080"
```

#### Test invalid YouTube URL (should return 400 error)
```bash
curl "http://localhost:8080?url=https://invalid.com&format=mp3"
```

#### Test invalid format (should return 400 error)
```bash
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&format=invalid"
```

#### Test valid conversion request
```bash
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&format=mp3"
```

#### Test different formats via API
```bash
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&format=mp4"
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&format=wav"
```

### Test 5.3: Stop API Server
```bash
# Find the PID
ps aux | grep yt2mp3-api | grep -v grep

# Kill it (replace 12345 with actual PID)
kill -9 12345
```

## Expected Test Results

### ✅ All Tests Pass When:

1. **CLI Tests**
   - Help message displays correctly
   - Invalid URLs are rejected
   - Invalid formats are rejected
   - Valid conversions create output files

2. **API Tests**
   - Server starts on port 8080
   - Missing parameters return 400 status
   - Invalid URLs return 400 status
   - Valid requests return 200 status with JSON response
   - Output files are created in correct directories

3. **Output Files**
   - MP3 files appear in `MP3/` directory
   - MP4 files appear in `MP4/` directory
   - WAV files appear in `WAV/` directory

## Checklist for Full Verification

```
Installation
  [ ] CMake installed successfully
  [ ] All C++ dependencies installed
  [ ] yt-dlp installed and working
  [ ] FFmpeg verified

Building
  [ ] cmake .. completed without errors
  [ ] make completed without errors
  [ ] yt2mp3-cli executable created
  [ ] yt2mp3-api executable created

CLI Testing
  [ ] Help command works
  [ ] Invalid URLs rejected
  [ ] Invalid formats rejected
  [ ] Conversion creates output files
  [ ] All three formats work (MP3, MP4, WAV)

API Testing
  [ ] Server starts without errors
  [ ] Server listens on port 8080
  [ ] Missing params return error
  [ ] Invalid URLs return error
  [ ] Valid requests return success
  [ ] Output files in correct directories

Documentation
  [ ] README.md is readable and complete
  [ ] CODE_OF_CONDUCT.md created
  [ ] ARCHITECTURE.md created
  [ ] CONTRIBUTING.md exists
```

## Common Issues & Solutions

| Problem | Cause | Solution |
|---------|-------|----------|
| CMake not found | Not installed | `sudo apt-get install cmake` |
| cpprestsdk not found | Not installed | `sudo apt-get install libcpprest-dev` |
| Boost not found | Not installed | `sudo apt-get install libboost-all-dev` |
| yt-dlp not found | Not installed | `pip install yt-dlp` or `sudo apt-get install yt-dlp` |
| 403 Forbidden error | yt-dlp outdated | `pip install --upgrade yt-dlp` |
| Port 8080 already in use | Another process using it | List processes: `lsof -i :8080` |
| Build fails | Old build directory | `rm -rf build && mkdir build && cd build` |
| Permission denied | Executable not executable | `chmod +x yt2mp3-cli yt2mp3-api` |

## Quick Test Script

```bash
#!/bin/bash
set -e

echo "🔨 Building YT-Converter..."
cd /home/ghost/Programming/YT-Converter
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)

echo "✅ Build successful!"
echo ""
echo "📋 Created executables:"
ls -lh yt2mp3-cli yt2mp3-api

echo ""
echo "🧪 Running basic tests..."
echo ""

echo "Test 1: CLI help"
./yt2mp3-cli --help 2>&1 | head -5

echo ""
echo "Test 2: CLI invalid URL"
./yt2mp3-cli "invalid" mp3 2>&1 || echo "[Expected error caught]"

echo ""
echo "✅ All basic tests completed!"
echo ""
echo "Next steps:"
echo "1. Run actual conversion: ./yt2mp3-cli \"<YouTube URL>\" mp3"
echo "2. Start API server: ./yt2mp3-api"
echo "3. Test API: curl \"http://localhost:8080?url=<URL>&format=mp3\""
```

Save as `test.sh`, make executable, and run:
```bash
chmod +x test.sh
./test.sh
```

