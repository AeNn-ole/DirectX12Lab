#include <windows.h>
#include <stdio.h>

int main()
{
    // Просто проверка, что проект собирается
    MessageBox(NULL, L"Тест успешен!", L"OK", MB_OK);

    // Проверка формата строки
    char buf[256];
    HRESULT hr = 0x80070002;  // Пример кода ошибки
    sprintf_s(buf, sizeof(buf), "Ошибка: 0x%08X", (unsigned)hr);
    MessageBoxA(NULL, buf, "Формат строки", MB_OK);

    return 0;
}