# devastator
Devastator Parallel Discrete Event Simulator

The Devastator runtime is a modern C++ implementation of optimistic parallel discrete event simulation methods.  Devastator allows simulation application code to productively specify their component and event functionality with C++14 constructs.  It utilizes GASNet-EX for distributed memory communication and includes parallel performance optimizations such as light-weight thread message queues and asynchronous GVT.  Furthermore, it supports efficient event broadcasts and pause-rewind-resume functionality to support periodic load balancing and outer loop optimization algorithms.
