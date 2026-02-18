pipeline {
    agent any

    environment {
        BUILD_DIR = "build"
    }

    stages {

        stage('Checkout') {
            steps {
                echo "Checking out source code..."
                checkout scm
            }
        }

        stage('Create Build Directory') {
            steps {
                sh 'mkdir -p $BUILD_DIR'
            }
        }

        stage('CMake Configure') {
            steps {
                sh '''
                    cd $BUILD_DIR
                    cmake ..
                '''
            }
        }

        stage('Build') {
            steps {
                sh '''
                    cd $BUILD_DIR
                    make
                '''
            }
        }

        stage('Install') {
            steps {
                sh '''
                    cd $BUILD_DIR
                    make install
                '''
            }
        }
    }

    post {
        success {
            echo "Build completed successfully!"
        }
        failure {
            echo "Build failed!"
        }
    }
}
