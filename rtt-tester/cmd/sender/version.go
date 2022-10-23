package main
/*
Copyright (c) 2022 Port 9 Labs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
import (
	"fmt"
	"os"
	"strings"
)

// Version of the executable
const Version = "0.1.0"

var (
	// BuildTime is the time of build - will be altered by the linker
	BuildTime string
	// Builder is the user that built this
	Builder string
	// GitRev is the git revision - will be altered by the linker
	GitRev string
	// BuildHost is the host name of the build machine
	BuildHost string
)

func mkunknown(s string) string {
	if s == "" {
		return "(unknown)"
	}
	return s
}

func init() {
	BuildTime = mkunknown(BuildTime)
	GitRev = mkunknown(GitRev)
	BuildHost = mkunknown(BuildHost)
	Builder = mkunknown(Builder)
}

// VersionString returns a version string for this app.
func VersionString() string {
	sp := strings.Split(os.Args[0], "/")
	appname := sp[len(sp)-1]
	return fmt.Sprint(appname, " v", Version, ", built on ", BuildHost, " by ", Builder, " at ", BuildTime, ", commit ", GitRev)
}
