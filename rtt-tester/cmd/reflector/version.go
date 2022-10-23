package main

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
