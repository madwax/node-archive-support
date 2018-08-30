Node.js Archive Addition Code

High Level View
------------------------------------------------------------------

This patch/code allows Node.js to mount into it's own process space (not a file system mount) an archive and access files stored within as if it was on the systems local file system.

This is possible as Node.js uses libuv (mostly) for all it's file IO so we replace all uv_fs_* functions with are own versions that workout if the filepath or file handle is to something within an archive or if not calls the real libuv function.

To use this patch you need to pass the following command line args to node:
* --archive.path %FILEPATH_TO_ARCHIVE_FILE% e.g. /tmp/my.sexy.app.zip
* --archive.mount %WERE_ARCHIVE_ROOT_APPEARS_IN_LOCAL_FILESYSTEM" e.g. /tmp/myapp 
* You need to pass the full filepath to your main script as it would be seen in the mounted file system e.g. /tmp/myapp/app.js


How Does It Work
------------------------------------------------------------------

There is a global archive manager object (so we could have multipal archives in the future) [archive::Manager].  

There is a Base Archive class (archive::Archive) which the manager uses as an interface to any derived archive types.

Currently we have only support Zip files (archive::ArchiveJUnzip).



