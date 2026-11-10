// Android (bionic) has strlcpy / strlcat / arc4random itself; what glibc's headers pull in on their own and bionic's
// don't is included here (via runtime/platform.h on Android only).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
