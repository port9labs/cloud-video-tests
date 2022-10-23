# cloud-video-tests

Code mentioned by the SMPTE paper *Uncompressed Video in the Public Cloud: Are We There Yet?*

This code was initially written by [Port 9 Labs](https://port9labs.com) to help explain some of the tests we make for network transmission of uncompressed video on public clouds and released as open source at the [*SMPTE Media Technology Summit*](https://summit.smpte.org) on October 25, 2022.

## Our results on various clouds

Port 9 Labs will periodically publish measurements made on several public clouds using this software. Look for posts on our [blog](http://blog.port9labs.com) tagged [testing](http://blog.port9labs.com/category/testing).

The main programs are:

### Windowed RTT

Look in the `rtt-tester` directory. [README](./rtt-tester/README.md)

### Flow measurements

Look in the `flow-tester` directory. [README](./flow-tester/README.md)

## TODO

These programs generate sqlite2 files containing their measurements. We intend to add some R scripts that will make plots of the interesting parts of this data.
