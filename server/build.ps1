$env:CGO_ENABLED='0'; $env:GOOS='linux'; $env:GOARCH='386'
go build -trimpath -ldflags="-s -w" -o clockreader-server-linux-386 .