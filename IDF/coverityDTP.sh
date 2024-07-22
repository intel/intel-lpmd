echo "Starting Coverity Scan"


export PATH=$PATH:/OWR/Tools/coverity/2023.3.4/bin
cd /home/jenkins/agent/workspace/DTPWS/DTP/package/buildScript
chmod +x build.sh
echo "Calling cov config"
cov-configure -c covconfig.xml --gcc
cat covconfig.xml
echo "calling cov build"
cov-build -c covconfig.xml --dir coverityreport ./build.sh

echo "Coverity Build Completed"
