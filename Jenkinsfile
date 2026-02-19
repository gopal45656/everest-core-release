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

        stage('Cross Compile (Clean + Build)') {
            steps {
                sh '''
                    bash -c "
                    set -e

                    echo '--------------------------------------'
                    echo 'Sourcing Yocto SDK Environment'
                    echo '--------------------------------------'
                    source $SDK_ENV

                    echo 'Compiler being used:'
                    echo \$CXX

		    if [ ! -d $BUILD_DIR ]; then
                        echo 'Build directory not found. Creating...'
                        mkdir -p $BUILD_DIR
                        cd $BUILD_DIR

		    echo '--------------------------------------'
                    echo 'Running CMake'
                    echo '--------------------------------------'
                    cmake ..

                    else
                        echo 'Build directory exists. Reusing...'
                    fi

	                echo '--------------------------------------'
                        echo 'Building'
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

