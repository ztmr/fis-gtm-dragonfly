GT.M for DragonFly/x86_64 (native port)

This port is based on the version for FreeBSD/x86_64
which was derived from GT.M source base for Linux/x86_64.

For further details, please read the FreeBSD port's
details since this document covers only DragonFly
differences against the FreeBSD.


Cookbook:

(0) tested on DragonFly 3.2-RELEASE x86_64

(1) packages: cmake, ncurses, libelf, icu,
              libgcrypt, libgpg-error, gpgme

(2) non-packaged requirements: libexecinfo

  (a) get libexecinfo tarball for FreeBSD:
      ftp://ftp.freebsd.org/pub/FreeBSD/distfiles/libexecinfo-1.1.tar.bz2

  (b) modify it like this:

    root@dfly:~/libexecinfo-1.1# undo -d execinfo.c
    diff -N -r -u execinfo.c@@0x00000001032ee540
    execinfo.c@@0x00000001032f6650 (to 29-Dec-2012 13:55:05)
    --- execinfo.c@@0x00000001032ee540      2012-12-29 13:53:51.378149000
    +0100
    +++ execinfo.c@@0x00000001032f6650      2012-12-29 13:54:53.008235000
    +0100
    @@ -78,7 +78,7 @@
         rval = malloc(clen);
         if (rval == NULL)
             return NULL;
    -    (char **)cp = &(rval[size]);
    +    cp = &(rval[size]);
         for (i = 0; i < size; i++) {
             if (dladdr(buffer[i], &info) != 0) {
                 if (info.dli_sname == NULL)
    root@dfly:~/libexecinfo-1.1# 

(3) go to the GT.M source directory

(4) ensure you have UTF-8 locale (C/US-ASCII won't
    build *.m routines correctly!)
    You can do: setenv LC_ALL en_US.UTF-8

(4) mkdir build ; cmake ../ ; make ; make install

(5) You should have GT.M installed in:
      /usr/local/lib/fis-gtm/V6.0-000_x86_64/

...then you can work with GT.M as you've used
to on another platforms:

(1) set environment variables
  $ setenv gtm_dist /usr/local/lib/fis-gtm/V6.0-000_x86_64
  $ setenv gtmgbldir demo_db
  $ setenv gtmroutines ". $gtm_dist"

(2) create database
  $ $gtm_dist/mumps -r GDE
  GDE> exit
  $ $gtm_dist/mupip create

(3) enter direct mode:
  $ $gtm_dist/mumps -di
  GTM>
  -or-
  run your MUMPS code:
  $ $gtm_dist/mumps -r SHOW^ZTMR


KNOWN ISSUES:

- for some reason, there is problem with maximal subcsript length:
  (even if the MAX_GVSUBSCRIPTS=32 is set in the code as usual)

    GTM>s ^xyz(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)=1
    %GTM-E-GVSUBOFLOW, Maximum combined length of subscripts exceeded
    %GTM-I-GVIS,            Global variable:
    %^xyz(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19*
    
    GTM>s ^xyz(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19)=1
    GTM>w ^xyz(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19)
    1
    GTM>
  ...this problem was on FreeBSD too, but it somehow "fixed itself"
  when doing last porting iteration so we don't recall what's the
  real reason of this GVSUBOFLOW issue :-(

- GT.M replication has not been tested yet

- GT.CM has not been tested and probably won't work like on the FreeBSD
