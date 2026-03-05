#include <windows.h>
#include <stdio.h>

int main()
{
    
    MessageBox(NULL, L"Test Confirmed!", L"OK", MB_OK);

   
    char buf[256];
    HRESULT hr = 0x80070002;  
    sprintf_s(buf, sizeof(buf), "Error: 0x%08X", (unsigned)hr);
    MessageBoxA(NULL, buf, "String format", MB_OK);

    return 0;
}