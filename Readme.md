# Fetching / Updating the Protocol Files

The protos in this directory come from two places.

The main Google API protos come from the 
[github googleapis](https://github.com/googleapis/googleapis) repository. You should run `proto_update.sh` to re-download these 
and refresh them.

As the dialogflow API protos are now GA they are updated / installed using the same script.

# Building the library

1. Run the `install_protoc.sh` script to download, compile, and install
   * `protoc` (the protocol specification compiler)
   * the `protobuf` library
   * the `gRPC` library
1. Run `make` to:
   1. Build the protobuffer code for DialogFlow
   1. Compile the protobuffer and the shim code to `libdfegrpc.so`
   1. Compile the test client
1. Run `make install` to install the library and header files.

# Running the test client

To build the test client, run `make test`.

To run the test client, execute:

```./test_client```
