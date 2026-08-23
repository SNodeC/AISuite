if(NOT DEFINED OPENSSL_EXECUTABLE OR NOT EXISTS "${OPENSSL_EXECUTABLE}")
    message(FATAL_ERROR "OPENSSL_EXECUTABLE is required")
endif()
if(NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "OUTPUT_DIRECTORY is required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
set(config "${OUTPUT_DIRECTORY}/openssl.cnf")
set(certificate "${OUTPUT_DIRECTORY}/loopback-cert.pem")
set(key "${OUTPUT_DIRECTORY}/loopback-key.pem")
file(REMOVE "${certificate}" "${key}")
file(
    WRITE "${config}"
    [=[
[req]
distinguished_name = subject
x509_extensions = extensions
prompt = no

[subject]
CN = localhost

[extensions]
subjectAltName = @alternative_names
basicConstraints = critical,CA:TRUE
keyUsage = critical,digitalSignature,keyEncipherment,keyCertSign
extendedKeyUsage = serverAuth

[alternative_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
IP.2 = ::1
]=]
)

execute_process(
    COMMAND
        "${OPENSSL_EXECUTABLE}" req -x509 -newkey rsa:2048 -sha256 -nodes
        -days 1 -config "${config}" -keyout "${key}" -out "${certificate}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "OpenSSL certificate generation failed: ${output}${error}")
endif()
if(NOT EXISTS "${certificate}" OR NOT EXISTS "${key}")
    message(FATAL_ERROR "OpenSSL did not create the expected certificate and key")
endif()
message(STATUS "codex2 communication trace: generated loopback TLS certificate=${certificate} key=${key}")
