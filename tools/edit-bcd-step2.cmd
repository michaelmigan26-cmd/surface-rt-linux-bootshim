@echo off
set BCD=F:\efi\microsoft\boot\bcd
set LOG=C:\jailbreak\edit-bcd2.log

echo === Re-pointing BCD to bootshim-test.efi === > "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

bcdedit /store %BCD% /set {default} path \efi\boot\bootshim-test.efi >> "%LOG%" 2>&1
bcdedit /store %BCD% /set {default} description "Boot-Shim Test (msm8994)" >> "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Verify --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /enum {default} >> "%LOG%" 2>&1
echo DONE >> "%LOG%" 2>&1
