#!/bin/bash
set -e
set -o pipefail
yum -y upgrade
yum -y install gcc-c++
