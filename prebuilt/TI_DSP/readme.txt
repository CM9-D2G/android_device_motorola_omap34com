The dsp binaries included in the "DSP" directory have been verified with the RLS25.INC3.4P3_RC0 release.
Visit: http://www.omappedia.org/wiki/Android_Getting_Started#Stable_Release for accessing
the source code with the above mentioned tag.

Please drop all of the dsp binaries from the DSP directory in to your /system/lib/dsp directory in your
target file system.  Please note that if the "dsp" directory is not present in your target file system it
will need to be created.  More setup related instructions are located here:
http://www.omappedia.org/wiki/Android_Getting_Started#Enabling_DSP_Codecs


If these flags are not enabled in either of the above mentioned files, rebuilding the file system may not be necessary, just copy the dsp binaries
into /system/lib/dsp/ and boot Android and exercise the various use cases.  AAC-dec, JPG-dec, JPEG-enc, MPEG4-dec, H264-dec have been verified.

For questions/feedback please email: omapzoom@googlegroups.com.
