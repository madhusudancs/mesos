#!/bin/bash
PYTHON=python
if [ "`uname`" == "SunOS" ]; then
  PYTHON=python2.6
fi
PYTHONPATH=`dirname $0`/../../src/swig/python $PYTHON ./torquesched.py $@