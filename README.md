# Terminator — A Custom Terminal

**Course:** Computing Lab (CS69201) · Term Project  
**Language:** C  
**Platform:** Linux with X11

## Overview

Terminator is a custom terminal emulator written in C. It provides an X11 graphical window for entering commands and viewing output, with tabs and shell-like command execution features. The implementation uses Linux/POSIX process and file-descriptor operations, together with Xlib for the graphical interface.

## Features

The project includes the following features:

- **X11 terminal interface:** graphical terminal window, command input/output, tab handling, and scrolling.
- **Command execution:** built-in commands and external programs launched with `fork()` and `execvp()`.
- **Pipes and redirection:** pipelines using `|`, input redirection with `<`, output redirection with `>`, and append redirection with `>>`.
- **Multiline input and quoting:** line continuation, single/double quotes, environment-variable expansion, and command substitution.
- **`multiWatch`:** runs multiple commands concurrently, displays their output periodically, and can be stopped with Ctrl+C.
- **Command-line editing hotkeys:** cursor movement, command-history navigation, deletion shortcuts, screen clearing, and copy/paste shortcuts.
- **Signals and job control:** Ctrl+C/Ctrl+Z handling and job-management commands such as `jobs`, `fg`, and `bg`.
- **Searchable command history:** saves command history in `~/.terminator_history` and supports Ctrl+R search.
- **File-name completion:** Tab completion for file names in the current tab's working directory.
- **Script execution:** support for running script files through the terminal's own command-processing implementation.

> Note: The exact behavior available in a build may depend on the current source code. See the source and test the required commands in your Linux environment before submission.

## Requirements

- Linux
- GCC or another compatible C compiler
- X11 development libraries and headers
- `make` (if building with the provided Makefile)

On Debian/Ubuntu-based systems, install the dependencies with:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

If the program uses any additional X11 libraries in your local build, install their corresponding development packages as well.

## Build

If the repository includes a `Makefile`, build the program from the repository directory:

```bash
make
```

The build should produce an executable named `terminator`.

If there is no Makefile, compile the C source manually. For example, if the source file is named `terminator.c`:

```bash
gcc -Wall -Wextra -o terminator terminator.c -lX11
```

Use the actual source filename in the repository. If your code requires additional libraries, include their linker flags.

## Run

Start the terminal in the default mode:

```bash
./terminator
```

The project specification also describes a process-mode invocation:

```bash
./terminator useProcess=True
```

Use this mode only if it is supported by the submitted build. The default invocation is the standard starting point.

The program needs access to an X11 display. If you are running Linux in a virtual machine, make sure its graphical desktop and display environment are available.

## Usage examples

Try these commands inside the Terminator window:

```bash
pwd
ls -l
echo "Hello, Terminator"
ls | wc -l
cat < input.txt
ls > output.txt
echo "another line" >> output.txt
history
jobs
multiWatch ["date", "ls | wc -l"]
```

For `multiWatch`, press **Ctrl+C** to stop monitoring and return to the prompt.

## Repository contents

The GitHub repository should include:

- The complete C source code.
- The compiled executable, if required by the submission instructions.
- A `Makefile` that builds `./terminator` (if required by the submission instructions).
- Screenshots demonstrating the application.
- This `README.md`.
- A design document, if required by the project specification.

## Known limitations

- The application depends on Linux/POSIX APIs and X11, so it is not a native Windows terminal application.
- Shell behavior may differ from Bash for syntax or features not explicitly implemented by this project.
- The precompiled executable may not work on another machine with a different architecture or library environment. Rebuilding from source is recommended.
- Validate each required feature in the submitted build before the final submission, especially execution modes, script handling, job control, and tab behavior.

## Academic project

This project was developed for **Computing Lab (CS69201)** as a term project. The project explores X11 GUI programming, process creation and control, pipes, file-descriptor redirection, command parsing, signal handling, and shell-like terminal behavior.
