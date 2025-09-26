# This is a makefile for building the GUI version of wangemu on Linux/OSX
# Requires wxWidgets development libraries
#
# make targets:
#
# make         -- shorthand for "make debug"
# make debug   -- non-optimized wangemu build, and generate tags
# make opt     -- optimized wangemu build, and generate tags
# make tags    -- make ctags index from files in src/
# make clean   -- remove all build products
# make release -- create a "release" directory containing the app bundle and support files
# make dmg     -- package the release files into a .dmg disk image

.PHONY: debug opt tags clean release dmg

# Add .d to Make's recognized suffixes.
.SUFFIXES: .c .cpp .mm .d .o

# don't create dependency files for these targets
NODEPS := clean tags

# Project root paths (makefile is now in project root)
SRCDIR := ./src
OBJDIR := ./obj

# Core source files (platform independent)
CORE_CPP_SOURCES := \
    $(SRCDIR)/core/cpu/Cpu2200t.cpp \
    $(SRCDIR)/core/cpu/Cpu2200vp.cpp \
    $(SRCDIR)/core/cpu/ucode_2200B.cpp \
    $(SRCDIR)/core/cpu/ucode_2200T.cpp \
    $(SRCDIR)/core/cpu/ucode_boot_vp.cpp \
    $(SRCDIR)/core/disk/DiskCtrlCfgState.cpp \
    $(SRCDIR)/core/disk/Wvd.cpp \
    $(SRCDIR)/core/io/IoCard.cpp \
    $(SRCDIR)/core/io/IoCardDisk.cpp \
    $(SRCDIR)/core/io/IoCardDisk_Controller.cpp \
    $(SRCDIR)/core/io/IoCardKeyboard.cpp \
    $(SRCDIR)/core/io/IoCardTermMux.cpp \
    $(SRCDIR)/core/system/error_table.cpp \
    $(SRCDIR)/core/system/Scheduler.cpp \
    $(SRCDIR)/core/system/system2200.cpp \
    $(SRCDIR)/core/util/dasm.cpp \
    $(SRCDIR)/core/util/dasm_vp.cpp

# Platform-specific sources for GUI/Windows
PLATFORM_CPP_SOURCES := \
    $(SRCDIR)/platform/common/SerialPort.cpp \
    $(SRCDIR)/platform/windows/host.cpp

# Shared configuration and script files
SHARED_CPP_SOURCES := \
    $(SRCDIR)/shared/config/CardInfo.cpp \
    $(SRCDIR)/shared/config/SysCfgState.cpp \
    $(SRCDIR)/shared/config/TermMuxCfgState.cpp \
    $(SRCDIR)/shared/script/ScriptFile.cpp \
    $(SRCDIR)/shared/terminal/Terminal.cpp

# Session files
SESSION_CPP_SOURCES := \
    $(SRCDIR)/headless/session/SerialTermSession.cpp

# GUI-specific files
GUI_CPP_SOURCES := \
    $(SRCDIR)/gui/system/UiSystem.cpp \
    $(SRCDIR)/gui/frames/UiCrtFrame.cpp \
    $(SRCDIR)/gui/frames/UiControlFrame.cpp \
    $(SRCDIR)/gui/frames/UiPrinterFrame.cpp \
    $(SRCDIR)/gui/dialogs/UiCrtConfigDlg.cpp \
    $(SRCDIR)/gui/dialogs/UiSystemConfigDlg.cpp \
    $(SRCDIR)/gui/dialogs/UiTermMuxCfgDlg.cpp \
    $(SRCDIR)/gui/dialogs/UiPrinterConfigDlg.cpp \
    $(SRCDIR)/gui/dialogs/UiDiskCtrlCfgDlg.cpp \
    $(SRCDIR)/gui/dialogs/UiCrtErrorDlg.cpp \
    $(SRCDIR)/gui/widgets/UiCrt.cpp \
    $(SRCDIR)/gui/widgets/UiCrtStatusBar.cpp \
    $(SRCDIR)/gui/widgets/UiCrt_Charset.cpp \
    $(SRCDIR)/gui/widgets/UiCrt_Keyboard.cpp \
    $(SRCDIR)/gui/widgets/UiCrt_Keyboard_martin.cpp \
    $(SRCDIR)/gui/widgets/UiCrt_Render.cpp \
    $(SRCDIR)/gui/widgets/UiPrinter.cpp \
    $(SRCDIR)/gui/widgets/UiMyStaticText.cpp \
    $(SRCDIR)/gui/widgets/UiMyAboutDlg.cpp \
    $(SRCDIR)/gui/widgets/UiDiskFactory.cpp \
    $(SRCDIR)/gui/widgets/IoCardDisplay.cpp \
    $(SRCDIR)/gui/widgets/IoCardPrinter.cpp

# C source files
C_SOURCES := \
    $(SRCDIR)/core/cpu/i8080.c \
    $(SRCDIR)/core/cpu/i8080_dasm.c

# MacOS-specific sources (if they exist)
MM_SOURCES := $(wildcard $(SRCDIR)/platform/common/osx_utils.mm)

# Combine all sources
ALL_CPP_SOURCES := $(CORE_CPP_SOURCES) $(PLATFORM_CPP_SOURCES) $(SHARED_CPP_SOURCES) $(SESSION_CPP_SOURCES) $(GUI_CPP_SOURCES)

# These are the dependency files, which make will clean up after it creates them
DEPFILES := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.d,$(ALL_CPP_SOURCES)) \
            $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.d,$(C_SOURCES)) \
            $(patsubst $(SRCDIR)/%.mm,$(OBJDIR)/%.d,$(MM_SOURCES))

OBJFILES := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(ALL_CPP_SOURCES)) \
            $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(C_SOURCES)) \
            $(patsubst $(SRCDIR)/%.mm,$(OBJDIR)/%.o,$(MM_SOURCES))

# debug build
debug: OPTFLAGS := -g -O0
debug: ./wangemu tags

# optimized build
opt: OPTFLAGS := -O2
opt: ./wangemu tags

CXX         := `wx-config --cxx`
CXXFLAGS    := `wx-config --cxxflags` -fno-common
CXXWARNINGS := -Wall -Wextra -Wshadow -Wformat -Wundef -Wstrict-aliasing=1 \
               -Wno-deprecated-declarations \
               -Wno-ctor-dtor-privacy -Woverloaded-virtual 
LDFLAGS     := `wx-config --libs`

# Create the executable
./wangemu: $(OBJFILES)
	$(CXX) -o $@ $(OBJFILES) $(LDFLAGS)

# don't create dependencies when we're cleaning, for instance
ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
    -include $(DEPFILES)
endif

# ===== create the dependency files =====

$(OBJDIR)/%.d: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MM -MT '$(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$<)' $< -MF $@

$(OBJDIR)/%.d: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MM -MT '$(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$<)' $< -MF $@

$(OBJDIR)/%.d: $(SRCDIR)/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MM -MT '$(patsubst $(SRCDIR)/%.mm,$(OBJDIR)/%.o,$<)' $< -MF $@

# ===== build the .o files =====

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(OBJDIR)/%.d
	@mkdir -p $(dir $@)
	$(CXX) $(OPTFLAGS) $(CXXFLAGS) $(CXXWARNINGS) -o $@ -c $<

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(OBJDIR)/%.d
	@mkdir -p $(dir $@)
	$(CXX) $(OPTFLAGS) $(CXXFLAGS) -o $@ -c $<

$(OBJDIR)/%.o: $(SRCDIR)/%.mm $(OBJDIR)/%.d
	@mkdir -p $(dir $@)
	$(CXX) $(OPTFLAGS) $(CXXFLAGS) $(CXXWARNINGS) -o $@ -c $<

# make a tags file
tags:
	find $(SRCDIR) \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.mm' \) | xargs ctags

clean:
	@echo "Cleaning GUI build artifacts"
	@rm -rf $(OBJDIR)
	@rm -f ./wangemu
	@rm -f ./tags

# These targets are primarily for MacOS but kept for compatibility
release: ./wangemu
	@echo "Release target not implemented for Linux build"

dmg: release  
	@echo "DMG target not implemented for Linux build"

# vim: ts=8:et:sw=4:smarttab