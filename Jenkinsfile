pipeline {
    agent any

    environment {
        BUILD_DIR = "build"
	SDK_ENV = "/var/lib/jenkins/tool_chain/environment-setup-aarch64-phytec-linux"
    }

    stages {

        stage('Checkout Source') {
            steps {
                checkout scm
            }
        }

        stage('Cross Compile (Incremental Build)') {
            steps {
                    echo '--------------------------------------'
                    echo 'WellCome to the CI/CD'
                    echo '--------------------------------------'
                sh '''
                    bash -c "
                    set -e

                    echo '--------------------------------------'
                    echo 'Sourcing Yocto SDK Environment'
                    echo '--------------------------------------'
                    source $SDK_ENV

                    echo 'Compiler being used:'
                    echo \$CXX

                    # Create build directory if it doesn't exist
                    if [ ! -d $BUILD_DIR ]; then
                        echo 'Build directory not found, creating...'
                        mkdir -p $BUILD_DIR
                    fi

                    cd $BUILD_DIR

                    # Check if CMake needs to rerun
                    if [ ! -f Makefile ] || [ ../CMakeLists.txt -nt Makefile ]; then
                        echo 'CMake needs to run (Makefile missing or CMakeLists.txt changed)...'
                        cmake ..
                    else
                        echo 'Skipping CMake (up-to-date)'
                    fi

                    echo '--------------------------------------'
                    echo 'Building (Incremental)'
                    echo '--------------------------------------'
                    make -j$(nproc)

                    echo '--------------------------------------'
                    echo 'Installing'
                    echo '--------------------------------------'
                    make install

                    echo '--------------------------------------'
                    echo 'Verifying Binary Architecture'
                    echo '--------------------------------------'
                    file *
                    "
                '''
            }
        }

        stage('Archive Artifacts') {
            steps {
		echo 'Archiving only installed artifacts from build/dist ...'
                archiveArtifacts artifacts: 'build/dist/**', fingerprint: true
            }
        }
    }

    post {
        success {
            echo 'ARM Cross Compilation Successful ✅'
        }
        failure {
            echo 'Build Failed ❌'
        }
    }
}
