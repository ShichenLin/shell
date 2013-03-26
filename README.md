# Potential bug and its fix:
Since printf() is not async-signal-safe(not reentrant), it shouldn't be used in signal handlers, but I used it, so it may lead to potential bugs like deadlock, etc. The fix is easy, just using write() sys-call instead. (But using write() for formatted output may be unbearable. I've searched google and a lot other references, but they just mention that vfprintf(), vsprintf(), vsnprintf()... these functions are just thread-safe, not sure whether they're async-signal-safe or not. So I'm not able to make formatted output easy in signal handlers myself. If you find any conclusion about the signal-reentrancy of these functions, please contact me by(siyuanh@andrew.cmu.edu), thanks very much.)


# File structure:
tsh.c
        All the functionalities of this unix-like tiny shell are in this file.

sdriver.c
        The shell driver source program

runtrace.c
	The trace interpreter source program

trace{00-24}.txt
	Trace files used by the driver

config.h
        Header file for sdriver.c

mycat.c
myenv.c
myintp.c
myints.c
myspin1.c
myspin2.c
mysplit.c
mysplitp.c
mytstpp.c
mytstps.c
	These are helper programs that are referenced in the trace files.
