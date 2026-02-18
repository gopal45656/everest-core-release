pipeline {
    agent any

    environment {
        BUILD_DIR = "build"
        SDK_ENV = "/home/sweetlin/Gopal/Phy_everest/tool_chain/environment-setup-aarch64-phytec-linux"
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

                    echo '--------------------------------------'
                    echo 'Cleaning old build directory'
                    echo '--------------------------------------'
                    rm -rf $BUILD_DIR
                    mkdir -p $BUILD_DIR
                    cd $BUILD_DIR

                    echo '--------------------------------------'
                    echo 'Running CMake'
                    echo '--------------------------------------'
                    cmake ..

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
                archiveArtifacts artifacts: 'build/**/*', fingerprint: true
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
