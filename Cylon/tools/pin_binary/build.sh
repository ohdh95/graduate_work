debug=""

if [ -n "$1" ]; then
    debug="-DDEBUG"
fi

cmd="gcc -shared -fPIC -o pin_binary.so pin_binary.c -ldl -lnuma $debug"
echo $cmd
$cmd
