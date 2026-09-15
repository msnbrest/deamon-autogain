pour compiler :

g++ -O2 -std=c++17 autogain.cpp -o autogain $(pkg-config --cflags --libs libpipewire-0.3)

pour verif qq info :

wpctl status -n
pw-link -l


restart service :

systemctl --user restart mini-autogain
