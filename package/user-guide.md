
Package sets up ILEO Intel Linux energy Optimier. Installation executes a script to build ILEO package from open source and execute ILEO as daemon. On system with tuneD support package adds custom profiles to activate and deactive ILEO. ILEO optimizes Intel SoC enegry settings set to achieve optimal  power and performance on Intel Ultra 1st Gen linux platforms.

Note: Limited validation on Ubuntu 24.04 LTS and Ubuntu 22.04 LTS with common workloads like video playback, video conference and web browsing.

# prerequisite
Please make sure the system is up to date by running "apt update" command before installing this package.
tuned - system tuning daemon (on supported distros, will be installed/updated by the program from distro repo).

# Deployment steps
    copy the package to target machine
    cd to the package folder
    ## format deb
        ### install
		        #### PPD (power-profiles-daemon) is actively running
					sudo dpkg -i <deb file name>
						
		        #### PPD (power-profiles-daemon) is not actively running		
				
					#### tuned already installed or tuned not supported on the platform.
						sudo dpkg -i <deb file name>
	 
					#### tuned not installed already but supported on the platform
						this package install dependencies [tuned] automatically.
						##### option 1
							sudo apt install tuned
							sudo dpkg -i <deb file name>
						##### option 2
							sudo dpkg -i <deb file name>
								-- results in failure. continue with below command.
							sudo apt-get -f install
        ### uninstall
            sudo dpkg -r <deb name>
		
    ## format tar
        ### install
            sudo tar -xvf <name>.gz.tar
            run: sudo ./deploy.sh
            
        ### uninstall
            run: sudo ./rollback.sh
            manually remove the untar folder
	
    ## uninstallation behaviour
		If tuned is required by this package:
			- after installation the current profile will be set to intel_hepo
			- package uninstallation will set the default tuned profile off
        
		Upon uninstallation,
			- the EPP value will be set to "balance_performance".
			- power-profiles-daemon service will be unmasked

# system daemon commands:
	sudo systemctl status intel_lpmd
	sudo systemctl start intel_lpmd
	sudo systemctl stop intel_lpmd	

# tuneD commands:

Ref: https://tuned-project.org/. visit tuneD website for more/latest information.

To switch profile [to activate new profile and deactivate current profile] use below command

"sudo tuned-adm profile <profile name>"
"sudo tuned-adm off" to turn off profile.
"tuned-adm active" to get lists the active profiles.

# License
tuneD releated files [configuration, profile scripts, ...] are under GPL-2.0-or-later license
others [setup scripts ...] are under Intel OBL license
intel's intel hybrid optimizer profile binary is under open source license. [https://github.com/intel/intel-lpmd]
