# ESP32-S2 Wi-Fi Devboard

This directory holds the passive session, microSD writer, and command handling.
Record the source revision and build settings when you set up the toolchain.

The [write helper](storage/pending_write.h) checks one write to an open file.
After a short write, keep the block and the number of bytes the file accepted.
Retry from the byte after that prefix. If the helper returns `invalid`, check
the file before retrying. T2 checks the saved file after closing it.
