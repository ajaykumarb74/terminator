#!/bin/bash
# comment
if [ -f "$1" ]; then
echo "File exists, compiling..."
else
echo "Source file missing!"
fi
