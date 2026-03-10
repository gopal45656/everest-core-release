pipeline {
    agent any

    environment {
        BUILD_DIR = "build"
        SDK_ENV = "/home/sweetlin/Gopal/Tool_chains/Cube/environment-setup-aarch64-phytec-linux"
    }

    stages {

        stage('Checkout Source') {
            steps {
                checkout scm
            }
        }

        stage('Cross Compile (Incremental Build)') {
            steps {
                sh '''
                set -e

                echo "--------------------------------------"
                echo "Cleaning conflicting environment"
                echo "--------------------------------------"

                unset CC
                unset CXX
                unset CPP
                unset LD
                unset AR
                unset STRIP
                unset CFLAGS
                unset CXXFLAGS
                unset LDFLAGS

                echo "--------------------------------------"
                echo "Sourcing Yocto SDK Environment"
                echo "--------------------------------------"

                source $SDK_ENV

                echo "Compiler being used:"
                echo $CC
                echo $CXX
                which $CXX

                echo "SYSROOT:"
                echo $SDKTARGETSYSROOT

                echo "--------------------------------------"
                echo "Preparing Build Directory"
                echo "--------------------------------------"

                if [ ! -d $BUILD_DIR ]; then
                    mkdir -p $BUILD_DIR
                fi

                cd $BUILD_DIR

                echo "--------------------------------------"
                echo "Running CMake if required"
                echo "--------------------------------------"

                if [ ! -f Makefile ] || [ ../CMakeLists.txt -nt Makefile ]; then
                    cmake .. \
                        -DCMAKE_SYSROOT=$SDKTARGETSYSROOT \
                        -DCMAKE_FIND_ROOT_PATH=$SDKTARGETSYSROOT \
                        -DCMAKE_C_COMPILER=$CC \
                        -DCMAKE_CXX_COMPILER=$CXX
                else
                    echo "Skipping CMake (Incremental Build)"
                fi

                echo "--------------------------------------"
                echo "Building"
                echo "--------------------------------------"

                make -j$(nproc)

                echo "--------------------------------------"
                echo "Installing"
                echo "--------------------------------------"

                make install

                echo "--------------------------------------"
                echo "Verifying Binary Architecture"
                echo "--------------------------------------"

                file *
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
