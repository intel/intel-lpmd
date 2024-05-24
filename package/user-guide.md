
Package contains tuneD custom profiles to set to achieve best power and performance on Intel Ultra 1st Gen linux platforms.
Note: Limited validation on Ubuntu 24.04 LTS and Ubuntu 22.04 LTS with common workloads like video playback, video conference and web browsing.

# prerequisite
Please make sure the system is up to date by running "apt update" command before installing this package.
tuned - system tuning daemon (will be installed by the program from ubuntu repo).

# Deployment steps
    copy the package to target machine
    cd to the package folder
    ## format deb
        ### install
                #### tuned already installed.
                    sudo dpkg -i <deb file name>
 
                #### tuned not installed already
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
            run: sudo ./remove.sh
            manually remove the untar folder
	
    ## uninstallation behaviour
	If tuned is installed by this package:
		- after installation the current profile will be set to intel-best_performance_mode
		- package uninstallation will automatically remove tuned
		- if tuned is installed as an dependency of the debian package, the uninstallation will not automatically remove tuned 
        
	If tuned was installed before this package:
		- if current profile already exists, profile intel-best_performance_mode will be added to the existing profile upon package installation
		- the package uninstallation will turn off the active profile (no current active profile).  
		- if the previous tuned profile information was not available, the current profile will be left untouched.

    Upon uninstallation,
        - the EPP value will be set to "balance_performance".
        - power-profiles-daemon service will be unmasked

# note: [tuneD conflicts with ppd on ubuntu]
    power-profiles-daemon[ppd] will be disabled when installing tuned. Choosing power profile option [ "performance", "balanced", or "power saver" ] won't be available in settings UI after disabling PPD service. Upon reboot, ppd service will take a priority to start and disable the Tuned service. This package installation will unmask power-profiles-daemon service so Tuned service and the profile activates properly after reboot.

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
