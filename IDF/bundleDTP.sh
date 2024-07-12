function execute_cmd {
    cmd=$1
        echo "-->> eval $cmd"
        eval $cmd
        err=$?
        if [ $err -ne 0 ]; then
            echo "<<- Executed Command: $cmd"
            echo "<<- Error: $err"
            exit $err
        fi
}

echo "Printing the current directory"

echo ================================
cd ../bundleScript
echo ================================

CURRENTDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $CURRENTDIR
echo ================================
echo "Calling the bundleScript"
echo ================================

execute_cmd "chmod +x bundle.sh"
execute_cmd "./bundle.sh"

echo "Completed....Building the pkgComponents"
echo ================================

CURRENTFILE="$( cd "$( dirname "$0" )" && pwd )"
echo $CURRENTFILE
execute_cmd "ls -lrt"
echo ================================
