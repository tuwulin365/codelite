# Build wxWidgets from sources
---

## Windows
---

CodeLite uses `MSYS2` for installing compiler and other tools:

- Prepare a working terminal with all the tools required [as described here][4]
- Open `MSYS2` terminal, and clone wxWidgets sources:

```bash
git clone https://github.com/wxWidgets/wxWidgets
cd wxWidgets
git submodule update --init
```

- For a `Release` build of wxWidgets, run this:

```bash
mkdir build-release
cd build-release
cmake .. -G"MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release                 \
         -DwxBUILD_DEBUG_LEVEL=0                                        \
         -DwxBUILD_MONOLITHIC=1 -DwxBUILD_SAMPLES=SOME -DwxUSE_STL=1    \
         -DCMAKE_INSTALL_PREFIX=$HOME/root
mingw32-make -j$(nproc) install
```

- If you need a `Debug` build of wxWidgets, run this command instead:

```bash
mkdir build-debug
cd build-debug
cmake .. -G"MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DwxBUILD_DEBUG_LEVEL=1 \
    -DwxBUILD_SAMPLES=SOME  -DwxBUILD_MONOLITHIC=1  -DwxUSE_STL=1             \
    -DCMAKE_INSTALL_PREFIX=$HOME/root  
mingw32-make -j$(nproc) install
```

## Linux
---

To build wxWidgets on you computer you will need these packages:

- The gtk development package: for GTK+2 it's often called `libgtk2.0-dev` or similar; for GTK3, `libgtk-3-dev`
- `pkg-config` (which usually comes with the gtk dev package)
- The `build-essential` package (or the relevant bit of it: `g++`, `make` etc)
- `git`
- `cmake`

Use the following command to install the prerequisites for `Ubuntu`:

```bash
sudo apt-get install libgtk-3-dev       \
                     pkg-config         \
                     build-essential    \
                     git                \
                     cmake              \
                     libsqlite3-dev     \
                     libssh-dev         \
                     libedit-dev        \
                     libhunspell-dev    \
                     xterm
```

```bash
mkdir -p $HOME/devl
cd $HOME/devl

# Checkout sources
git clone https://github.com/wxWidgets/wxWidgets
cd wxWidgets
git submodule update --init

mkdir -p build-release
cd build-release

../configure --disable-debug_flag --with-gtk=3 --enable-stl
make -j$(nproc) && sudo make install
```

## macOS
---

#### Prerequisites

- Install [Homebrew][1]
- Install `cmake`
- Install `git`
- Install latest Xcode from Apple
- Install the Command Line Tools (open `Xcode` &#8594;  `Preferences` &#8594;  `Downloads and install the command line tools`). This will place `clang`/`clang++` in the default locations `/usr/bin`
- [Download wxWidgets sources][2]

#### Build wxWidgets

```bash
mkdir -p $HOME/devl
cd $_
git clone https://github.com/wxWidgets/wxWidgets.git
cd wxWidgets
git submodule update --init
mkdir build-release
cd $_
../configure --enable-shared --enable-monolithic --with-osx_cocoa CXX='clang++ -std=c++17 -stdlib=libc++' CC=clang --disable-debug --disable-mediactrl --enable-stl --with-libtiff=no --enable-utf8
make -j$(sysctl -n hw.physicalcpu)
sudo make install
```

 [1]: https://brew.sh/
 [2]: https://wxwidgets.org/downloads/
 [3]: https://www.wxwidgets.org/downloads
 [4]: /build/mingw_builds/#prepare-a-working-environment
