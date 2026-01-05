# w-mterm
A wireless terminal device for headless servers running on MFS.

### compiny
Compiny is a companion app for converting a current terminal session to a w-mterm one. Works on POSIX.

### charlie
Charlie is the MCU that talks to the server/PC. It gets compiny's terminal data and sends it to the server. I am using a lolin d1 mini, but the code should work on any esp32.
Special configuration is needed on the server for charlie to work.

# License notes
mfslib itself is licensed as LGPL. This is what charlie uses. compiny is entirely BSD 3-clause.
