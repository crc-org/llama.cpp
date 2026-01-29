# .\build.windows-host\bin\Debug\llama-cli.exe  -m ..\models\smollm  -p "Hello world"
exec ./build.windows-wsl/bin/llama-cli --verbose -m  ../models/smollm  -p "Hello world" <<< "/exit"
