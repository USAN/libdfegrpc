# Fetching / Updating the Protocol Files

The protos in this directory come from two places.

The main Google API protos come from the 
[github googleapis](https://github.com/googleapis/googleapis) repository. You should run `proto_update.sh` to re-download these 
and refresh them.

The dialogflow API protos come from the private alpha (at this time, at least). You should extract them from a sample application 
(node.js is typically the best) and drop them here manually.

# Building the library

1. Start a fresh VM (Centos7.2)
1. Run the `install_protoc.sh` script to download, compile, and install
   * `protoc` (the protocol specification compiler)
   * the `protobuf` library
   * the `gRPC` library
1. Run `make` to:
   1. Build the protobuffer code for DialogFlow
   1. Compile the protobuffer and the shim code to `libdfegrpc.so`
   1. Compile the test client

# Running the test client

To run the test client, execute:

```LD_LIBRARY_PATH=/usr/local/lib:. ./test_client```

(you may be able to omit the `LD_LIBRARY_PATH` setting if you first run ldconfig)
