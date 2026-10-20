#include "qlcsingleinstance.h"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    QlcSingleInstance instance;
    if (!instance.allowed())
        return instance.exitCode();
    if (argc != 2)
        return 2;
    std::ofstream marker(argv[1]);
    marker << "started\n";
    marker.close();
    std::string input;
    std::getline(std::cin, input);
    return 0;
}
