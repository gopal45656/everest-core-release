pipeline {
    agent any

    environment {
        LANG             = "en_US.UTF-8"
        LC_ALL           = "en_US.UTF-8"
        BUILD_DIR        = "build"
        SDK_ENV          = "/home/sweetlin/jenkins/tool_chain/environment-setup-aarch64-phytec-linux"
        CPM_SOURCE_CACHE = "/home/sweetlin/jenkins/.cpm_cache"
        INSTALL_PREFIX   = "${WORKSPACE}/build/dist"
    }

    stages {

        stage('Prepare') {
            steps {
                sh '''
                    whoami
                    ls -l $SDK_ENV
                '''
            }
        }

        stage('Checkout Source') {
            steps {
                checkout scm
            }
        }

        stage('Install everest-cmake (once)') {
            // Pre-install everest-cmake so CMake finds it before configure starts.
            // This prevents CPMAddPackage being called before CPM is loaded (root cause of your error).
            steps {
                sh '''
                    bash -lc "
                        set -e
                        EVEREST_CMAKE_INSTALL=/home/sweetlin/jenkins/everest-cmake-install

                        if [ ! -f \$EVEREST_CMAKE_INSTALL/lib/cmake/everest-cmake/everest-cmakeConfig.cmake ]; then
                            echo 'Installing everest-cmake...'
                            rm -rf /tmp/everest-cmake-src /tmp/everest-cmake-build
                            git clone --depth 1 --branch v0.5.0 \
                                https://github.com/EVerest/everest-cmake.git \
                                /tmp/everest-cmake-src
                            cmake -S /tmp/everest-cmake-src \
                                  -B /tmp/everest-cmake-build \
                                  -DCMAKE_INSTALL_PREFIX=\$EVEREST_CMAKE_INSTALL
                            cmake --install /tmp/everest-cmake-build
                            echo 'everest-cmake installed.'
                        else
                            echo 'everest-cmake already installed, skipping.'
                        fi
                    "
                '''
            }
        }

        stage('Cross Compile') {
            steps {
                // Using bash -lc (login shell) same as your working Yocto Jenkinsfile
                // so that 'source' and all env paths work correctly
                sh '''
                    bash -lc "
                        set -e
                        export LANG=en_US.UTF-8
                        export LC_ALL=en_US.UTF-8
                        export CPM_SOURCE_CACHE=/home/sweetlin/jenkins/.cpm_cache

                        echo '--------------------------------------'
                        echo 'Sourcing Yocto SDK Environment'
                        echo '--------------------------------------'
                        source $SDK_ENV

                        echo 'Compiler being used:'
                        echo \$CXX

                        echo '--------------------------------------'
                        echo 'Clean old build to avoid stale CMake cache'
                        echo '--------------------------------------'
                        rm -rf $BUILD_DIR
                        mkdir -p $BUILD_DIR
                        cd $BUILD_DIR

                        echo '--------------------------------------'
                        echo 'CMake Configure'
                        echo '--------------------------------------'
                        cmake .. \
                            -DCMAKE_PREFIX_PATH=/home/sweetlin/jenkins/everest-cmake-install \
                            -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX \
                            -DCPM_SOURCE_CACHE=\$CPM_SOURCE_CACHE

                        echo '--------------------------------------'
                        echo 'Building'
                        echo '--------------------------------------'
                        make -j\$(nproc)

                        echo '--------------------------------------'
                        echo 'Installing to build/dist'
                        echo '--------------------------------------'
                        make install

                        echo '--------------------------------------'
                        echo 'Verifying Binary Architecture'
                        echo '--------------------------------------'
                        find $INSTALL_PREFIX -type f | xargs file 2>/dev/null || true
                    "
                '''
            }
        }

        stage('Archive Artifacts') {
            steps {
                echo 'Archiving artifacts from build/dist...'
                archiveArtifacts artifacts: 'build/dist/**', fingerprint: true, allowEmptyArchive: true
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
