@echo off
set BCD=F:\efi\microsoft\boot\bcd
set LOG=C:\jailbreak\edit-bcd.log

echo === Editing BCD: %BCD% === > "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Change {default} path to shell.efi --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /set {default} path \efi\boot\shell.efi >> "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Change {default} description to "Boot Linux" --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /set {default} description "Boot Linux" >> "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Remove loadoptions (not needed for Linux) --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /deletevalue {default} loadoptions >> "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Verify {default} entry --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /enum {default} >> "%LOG%" 2>&1
echo. >> "%LOG%" 2>&1

echo --- Full BCD --- >> "%LOG%" 2>&1
bcdedit /store %BCD% /enum all >> "%LOG%" 2>&1

echo DONE >> "%LOG%" 2>&1
