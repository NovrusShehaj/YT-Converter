# Contributing to YT-Converter

Thank you for your interest in contributing to YT-Converter! We welcome contributions from the community and appreciate your help in making this project better. Please take a moment to review this document before making your contribution.

## Code of Conduct

This project adheres to the Contributor Covenant [Code of Conduct](./CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to the project maintainers.

## How Can I Contribute?

### Reporting Bugs

Before creating bug reports, please check the [issue list](https://github.com/yourusername/YT-Converter/issues) as you might find out that you don't need to create one. When you are creating a bug report, please include as many details as possible:

* **Use a clear and descriptive title**
* **Describe the exact steps which reproduce the problem**
* **Provide specific examples to demonstrate the steps**
* **Describe the behavior you observed after following the steps**
* **Explain which behavior you expected to see instead and why**
* **Include screenshots and animated GIFs if possible**
* **Include your environment details:**
  - Operating System and version
  - C++ compiler version
  - yt-dlp version (`yt-dlp --version`)
  - ffmpeg version (`ffmpeg -version`)
  - Boost library version
  - cpprestsdk version

### Suggesting Enhancements

Enhancement suggestions are tracked as GitHub issues. When creating an enhancement suggestion, please include:

* **Use a clear and descriptive title**
* **Provide a step-by-step description of the suggested enhancement**
* **Provide specific examples to demonstrate the steps**
* **Describe the current behavior and explain the expected behavior**
* **Explain why this enhancement would be useful**
* **List some other applications where this enhancement exists, if applicable**

### Pull Requests

* Follow the [C++ Style Guidelines](#c-style-guidelines)
* Include appropriate test cases for new features
* Update documentation to reflect any changes
* End all files with a newline
* Avoid platform-specific code when possible

## Development Setup

### Prerequisites

Ensure you have the following installed:

* **C++ Compiler**: GCC 7+, Clang 5+, or MSVC 2017+
* **CMake**: Version 3.10 or later
* **yt-dlp**: Latest version
* **ffmpeg**: Latest version
* **Boost**: Version 1.66 or later
* **cpprestsdk**: Version 2.10.0 or later
* **OpenSSL**: Version 1.1.0 or later
* **Git**: For version control

### Building from Source

1. **Clone the repository**
   ```bash
   git clone https://github.com/yourusername/YT-Converter.git
   cd YT-Converter
   ```

2. **Create a build directory**
   ```bash
   mkdir build
   cd build
   ```

3. **Configure the build**
   ```bash
   cmake ..
   ```

4. **Build the project**
   ```bash
   cmake --build .
   ```

5. **Run the tests** (if applicable)
   ```bash
   ctest
   ```

### Running Locally

**CLI Version:**
```bash
./yt2mp3 "https://www.youtube.com/watch?v=VIDEO_ID" mp3
```

**API Version:**
```bash
./yt2mp3-API
```

Then test with:
```bash
curl "http://localhost:8080?url=https://www.youtube.com/watch?v=VIDEO_ID&format=mp3"
```

## C++ Style Guidelines

### Naming Conventions

* **Classes and Structures**: Use PascalCase
  ```cpp
  class VideoConverter { };
  struct ConversionOptions { };
  ```

* **Functions and Methods**: Use camelCase
  ```cpp
  bool isValidURL(const std::string& url);
  void processVideo(const std::string& url);
  ```

* **Constants**: Use UPPER_SNAKE_CASE
  ```cpp
  const int MAX_RETRY_ATTEMPTS = 3;
  const std::string DEFAULT_FORMAT = "mp3";
  ```

* **Member Variables**: Use m_ prefix for private members
  ```cpp
  private:
      std::string m_videoId;
      int m_retryCount;
  ```

* **Local Variables**: Use camelCase
  ```cpp
  std::string outputPath = generatePath(videoId);
  ```

### Code Formatting

* **Indentation**: Use 4 spaces (not tabs)
* **Line Length**: Keep lines under 100 characters when possible
* **Braces**: Use Allman style
  ```cpp
  if (condition)
  {
      doSomething();
  }
  else
  {
      doSomethingElse();
  }
  ```

* **Spacing**: 
  - One space after control flow keywords (if, while, for)
  - No space between function name and parentheses
  - Spaces around binary operators

### Documentation

* **File Headers**: Include file purpose and author information
  ```cpp
  // VideoConverter.cpp
  // Handles video downloading and conversion operations
  // Author: Your Name
  ```

* **Function Documentation**: Use clear comments for public functions
  ```cpp
  /// Validates if the provided URL is a valid YouTube URL
  /// @param url The URL string to validate
  /// @return true if valid YouTube URL, false otherwise
  bool isValidURL(const std::string& url);
  ```

* **Complex Logic**: Add inline comments explaining the "why"
  ```cpp
  // Use yt-dlp instead of youtube-dl for better compatibility
  executeCommand("yt-dlp ...");
  ```

### Best Practices

* **Use const**: Mark parameters and methods that don't modify state as const
  ```cpp
  bool isValidFormat(const std::string& format) const;
  ```

* **Error Handling**: Use exceptions for error handling
  ```cpp
  try
  {
      downloadVideo(url, videoId);
  }
  catch (const std::exception& e)
  {
      std::cerr << "Error: " << e.what() << std::endl;
  }
  ```

* **Resource Management**: Use RAII pattern when possible
  ```cpp
  {
      FileHandle file(filename);
      // File is automatically closed when file goes out of scope
  }
  ```

* **Avoid Global Variables**: Keep code modular and testable
* **Single Responsibility**: Each function should do one thing well
* **DRY Principle**: Don't Repeat Yourself - extract common code into functions

## Commit Messages

* Use clear, descriptive commit messages
* Start with a verb (Add, Fix, Update, Remove, Refactor)
* Reference issue numbers when applicable

Examples:
```
Add support for WAV conversion format
Fix memory leak in VideoConverter class
Update error handling in API endpoints
```

## Testing

* Write tests for new features
* Ensure all tests pass before submitting a PR
* Test on multiple platforms when possible
* Include both positive and negative test cases

Example test:
```cpp
void testIsValidURL()
{
    assert(isValidURL("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    assert(!isValidURL("https://example.com"));
    assert(!isValidURL("not a url"));
}
```

## Pull Request Process

1. **Fork the repository**

2. **Create a feature branch**
   ```bash
   git checkout -b feature/amazing-feature
   ```

3. **Make your changes** following the style guidelines

4. **Commit your changes** with clear messages
   ```bash
   git commit -m "Add amazing feature"
   ```

5. **Push to your fork**
   ```bash
   git push origin feature/amazing-feature
   ```

6. **Create a Pull Request**
   - Use a clear, descriptive title
   - Reference related issues
   - Describe the changes made
   - Include any relevant screenshots or test results
   - Ensure CI/CD checks pass

7. **Respond to feedback** from reviewers
   - Make requested changes
   - Commit new changes with descriptive messages
   - Push updates to the same branch

### PR Review Checklist

Before submitting, ensure:
- [ ] Code follows style guidelines
- [ ] Comments added for complex logic
- [ ] Documentation updated
- [ ] Tests added/updated
- [ ] No new warnings introduced
- [ ] All tests pass locally
- [ ] Commit messages are clear
- [ ] No unrelated changes included

## Additional Notes

### Project Structure

Understanding the project layout will help with contributions:

```
YT-Converter/
├── src/
│   ├── cli/              # Command-line interface
│   ├── api/              # REST API implementation
│   ├── core/             # Core conversion logic
│   └── utils/            # Utility functions
├── include/              # Header files
├── tests/                # Test files
├── docs/                 # Documentation
├── CMakeLists.txt        # Build configuration
├── README.md             # Project README
├── LICENSE               # MIT License
└── CONTRIBUTING.md       # This file
```

### Getting Help

* Check the [README](./README.md) for usage information
* Review existing [issues](https://github.com/yourusername/YT-Converter/issues) and [discussions](https://github.com/yourusername/YT-Converter/discussions)
* Ask questions in GitHub Discussions
* Read the [Architecture](./docs/ARCHITECTURE.md) document for technical details

### Recognition

Contributors will be:
* Listed in the project README acknowledgments section
* Credited in release notes for significant contributions
* Given credit in commit history

## Questions?

Feel free to open an issue with the label "question" or start a discussion in the GitHub Discussions tab.

Thank you for contributing to YT-Converter! 🎉