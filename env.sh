#!/usr/bin/env bash

SCALEHLS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
if [ -n "${PATH}" ]; then
  export PATH=$PATH:$SCALEHLS_DIR/build/bin:$SCALEHLS_DIR/polygeist/build/bin
else
  export PATH=$SCALEHLS_DIR/build/bin:$SCALEHLS_DIR/polygeist/build/bin
fi
if [ -n "${PYTHONPATH}" ]; then
  export PYTHONPATH=$PYTHONPATH:$SCALEHLS_DIR/build/tools/scalehls/python_packages/scalehls_core
else
  export PYTHONPATH=$SCALEHLS_DIR/build/tools/scalehls/python_packages/scalehls_core
fi