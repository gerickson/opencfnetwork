               Changes and What's New in OpenCFNetwork
               ---------------------------------------

2022-01-31 v129.20.4

  * Addressed an issue on Linux in which a newly-connected TCP socket
    stream could signal being ready for reading but, in practice, was
    actually still connecting and would return EAGAIN on
    read/receive. Previously, threads or processes employing such a
    stream may time out or deadlock waiting for data. Now, such
    contexts should successfully make forward progress.

2021-11-11 v129.20.3

  * Addressed an issue in which a numeric host-to-address (for
    example, 127.0.0.1) lookup would fail.

2021-11-09 v129.20.2

  * Addressed an issue in which numeric host, 'localhost', or other
    cached- or local-file-resolved lookups would deadlock on lookup
    finalization.

2021-07-19 v129.20.1

  * Initial publish and release to GitHub.

