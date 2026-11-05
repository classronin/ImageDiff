
```
rc.exe icon.rc

cl.exe main.cpp /utf-8 /EHsc /O2 /I "E:\Programs\vcpkg\installed\x64-windows-static\include" /link /LIBPATH:"E:\Programs\vcpkg\installed\x64-windows-static\lib" glfw3.lib opengl32.lib user32.lib gdi32.lib shell32.lib winmm.lib ole32.lib windowscodecs.lib icon.res /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:ImageDiff.exe
```
