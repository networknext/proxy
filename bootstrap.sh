#!/bin/bash

# Bump up the max socket read and write buffer sizes
sudo sysctl -w net.core.rmem_max=1000000000
sudo sysctl -w net.core.wmem_max=1000000000
