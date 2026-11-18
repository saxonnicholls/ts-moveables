// server.go - the external grader for http::client's HTTP/2: Go's standard library server, which is a separate,
// widely deployed implementation, serving h2c (prior knowledge) and h2 over TLS with ALPN. Started by
// scripts/h2_client_interop.sh; prints "h2c=PORT tls=PORT" once both listen, and writes its self-signed
// certificate to -ca for the client to trust.
//
// Copyright 2010-2026 Saxon Herschel Nicholls
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"flag"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

func handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprintf(w, "hello %s %s", r.Proto, r.Header.Get("User-Agent"))
	})
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write(body)
	})
	mux.HandleFunc("/big", func(w http.ResponseWriter, r *http.Request) {
		n, _ := strconv.Atoi(r.URL.Query().Get("n"))
		chunk := []byte(strings.Repeat("z", 64*1024))
		for n > 0 {
			k := n
			if k > len(chunk) {
				k = len(chunk)
			}
			w.Write(chunk[:k])
			n -= k
		}
	})
	// gRPC's shape: a body, then the status in trailers
	mux.HandleFunc("/trailers", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Trailer", "Grpc-Status, Grpc-Message")
		w.Header().Set("Content-Type", "application/grpc")
		w.WriteHeader(200)
		w.Write([]byte("\x00\x00\x00\x00\x02ok"))
		w.Header().Set("Grpc-Status", "0")
		w.Header().Set("Grpc-Message", "all good")
	})
	mux.HandleFunc("/delay", func(w http.ResponseWriter, r *http.Request) {
		ms, _ := strconv.Atoi(r.URL.Query().Get("ms"))
		time.Sleep(time.Duration(ms) * time.Millisecond)
		fmt.Fprint(w, r.URL.Query().Get("id"))
	})
	// Over HTTP/2 this resets the stream with INTERNAL_ERROR
	mux.HandleFunc("/abort", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(200)
		w.(http.Flusher).Flush()
		panic(http.ErrAbortHandler)
	})
	mux.HandleFunc("/nocontent", func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) })
	return mux
}

func selfSigned(caPath string) tls.Certificate {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		panic(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "localhost"},
		DNSNames:              []string{"localhost"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IsCA:                  true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	if err := os.WriteFile(caPath, certPEM, 0o600); err != nil {
		panic(err)
	}
	keyDER, _ := x509.MarshalECPrivateKey(key)
	cert, err := tls.X509KeyPair(certPEM, pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER}))
	if err != nil {
		panic(err)
	}
	return cert
}

func main() {
	caPath := flag.String("ca", "h2_interop_ca.pem", "where to write the certificate the client should trust")
	flag.Parse()

	plain, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	secure, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}

	var h2c http.Protocols
	h2c.SetHTTP1(true)
	h2c.SetUnencryptedHTTP2(true)
	plainSrv := &http.Server{Handler: handler(), Protocols: &h2c}

	tlsSrv := &http.Server{Handler: handler(), TLSConfig: &tls.Config{Certificates: []tls.Certificate{selfSigned(*caPath)}}}

	go plainSrv.Serve(plain)
	go tlsSrv.ServeTLS(secure, "", "")
	fmt.Printf("h2c=%d tls=%d\n", plain.Addr().(*net.TCPAddr).Port, secure.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()
	select {}
}
