#include "config.h"

#include "certificate.hpp"
#include "certs_manager.hpp"
#include "csr.hpp"

#include <openssl/bio.h>
#include <openssl/ossl_typ.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <sdbusplus/bus.hpp>
#include <sdeventplus/event.hpp>
#include <string>
#include <utility>
#include <vector>
#include <xyz/openbmc_project/Certs/error.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <gtest/gtest.h>

namespace phosphor::certs
{
namespace
{
namespace fs = std::filesystem;
using ::sdbusplus::xyz::openbmc_project::Certs::Error::InvalidCertificate;
using ::sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

/**
 * Class to generate certificate file and test verification of certificate file
 */
class TestCertificates : public ::testing::Test
{
  public:
    TestCertificates() : bus(sdbusplus::bus::new_default())
    {
    }
    void SetUp() override
    {
        char dirTemplate[] = "/tmp/FakeCerts.XXXXXX";
        auto dirPtr = mkdtemp(dirTemplate);
        if (dirPtr == nullptr)
        {
            throw std::bad_alloc();
        }
        certDir = std::string(dirPtr) + "/certs";
        fs::create_directories(certDir);

        createNewCertificate();
    }

    void TearDown() override
    {
        fs::remove_all(certDir);
        fs::remove(certificateFile);
        fs::remove(CSRFile);
        fs::remove(privateKeyFile);
        fs::remove_all("demoCA");
    }

    void createNewCertificate(bool setNewCertId = false)
    {
        certificateFile = "cert.pem";
        CSRFile = "domain.csr";
        privateKeyFile = "privkey.pem";
        rsaPrivateKeyFilePath = certDir + "/.rsaprivkey.pem";
        std::string cmd = "openssl req -x509 -sha256 -newkey rsa:2048 ";
        cmd += "-keyout cert.pem -out cert.pem -days 365000 -nodes";
        cmd += " -subj /O=openbmc-project.xyz/CN=localhost";

        if (setNewCertId)
        {
            cmd += std::to_string(certId++);
        }

        auto val = std::system(cmd.c_str());
        if (val)
        {
            std::cout << "COMMAND Error: " << val << std::endl;
        }
    }

    void createNeverExpiredRootCertificate()
    {
        // remove the old cert
        fs::remove(certificateFile);

        // The following routines create a cert that has NotBefore
        // set to 1970/01/01 and NotAfter set to 9999/12/31 via the
        // OpenSSL CA application.
        certificateFile = "cert.pem";
        ASSERT_EQ(std::system("mkdir -p demoCA"), 0);
        ASSERT_EQ(std::system("mkdir -p demoCA/private/"), 0);
        ASSERT_EQ(std::system("mkdir -p demoCA/newcerts/"), 0);
        ASSERT_EQ(std::system("touch demoCA/index.txt"), 0);
        ASSERT_EQ(std::system("echo 1000 > demoCA/serial"), 0);
        ASSERT_EQ(
            std::system(
                "openssl req -x509 -sha256 -newkey rsa:2048 -keyout "
                "demoCA/private/cakey.pem -out demoCA/cacert.pem -nodes "
                "-subj /O=openbmc-project.xyz/C=US/ST=CA/CN=localhost-ca"),
            0);
        ASSERT_EQ(std::system(
                      "openssl req -new -newkey rsa:2048 -nodes -keyout "
                      "demoCA/server.key -out demoCA/server.csr -subj "
                      "/O=openbmc-project.xyz/C=US/ST=CA/CN=localhost-server"),
                  0);
        ASSERT_EQ(
            std::system(
                "openssl ca -batch -startdate 19700101000000Z -enddate "
                "99991231235959Z -out cert.pem -infiles demoCA/server.csr"),
            0);
    }

    bool compareFiles(const std::string& file1, const std::string& file2)
    {
        std::ifstream f1(file1, std::ifstream::binary | std::ifstream::ate);
        std::ifstream f2(file2, std::ifstream::binary | std::ifstream::ate);

        if (f1.fail() || f2.fail())
        {
            return false; // file problem
        }

        if (f1.tellg() != f2.tellg())
        {
            return false; // size mismatch
        }

        // seek back to beginning and use std::equal to compare contents
        f1.seekg(0, std::ifstream::beg);
        f2.seekg(0, std::ifstream::beg);
        return std::equal(std::istreambuf_iterator<char>(f1.rdbuf()),
                          std::istreambuf_iterator<char>(),
                          std::istreambuf_iterator<char>(f2.rdbuf()));
    }

    std::string getCertSubjectNameHash(const std::string& certFilePath)
    {
        std::unique_ptr<X509, decltype(&::X509_free)> cert(X509_new(),
                                                           ::X509_free);
        if (!cert)
        {
            std::string();
        }

        std::unique_ptr<BIO, decltype(&::BIO_free)> bioCert(
            BIO_new_file(certFilePath.c_str(), "rb"), ::BIO_free);
        if (!bioCert)
        {
            std::string();
        }

        X509* x509 = cert.get();
        if (!PEM_read_bio_X509(bioCert.get(), &x509, nullptr, nullptr))
        {
            std::string();
        }

        unsigned long hash = X509_subject_name_hash(cert.get());
        static constexpr auto AUTH_CERT_HASH_LENGTH = 9;
        char hashBuf[AUTH_CERT_HASH_LENGTH];
        sprintf(hashBuf, "%08lx", hash);
        return std::string(hashBuf);
    }

  protected:
    sdbusplus::bus::bus bus;
    std::string certificateFile, CSRFile, privateKeyFile, rsaPrivateKeyFilePath;

    std::string certDir;
    uint64_t certId;
};

class MainApp
{
  public:
    MainApp(phosphor::certs::Manager* manager,
            phosphor::certs::CSR* csr = nullptr) :
        manager(manager),
        csr_(csr)
    {
    }
    void install(std::string& path)
    {
        manager->install(path);
    }
    void delete_()
    {
        manager->deleteAll();
    }

    std::string generateCSR(std::vector<std::string> alternativeNames,
                            std::string challengePassword, std::string city,
                            std::string commonName, std::string contactPerson,
                            std::string country, std::string email,
                            std::string givenName, std::string initials,
                            int64_t keyBitLength, std::string keyCurveId,
                            std::string keyPairAlgorithm,
                            std::vector<std::string> keyUsage,
                            std::string organization,
                            std::string organizationalUnit, std::string state,
                            std::string surname, std::string unstructuredName)
    {
        return (manager->generateCSR(
            alternativeNames, challengePassword, city, commonName,
            contactPerson, country, email, givenName, initials, keyBitLength,
            keyCurveId, keyPairAlgorithm, keyUsage, organization,
            organizationalUnit, state, surname, unstructuredName));
    }
    std::string csr()
    {
        return (csr_->csr());
    }
    phosphor::certs::Manager* manager;
    phosphor::certs::CSR* csr_;
};

/** @brief Check if server install routine is invoked for server setup
 */
TEST_F(TestCertificates, InvokeServerInstall)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
}

/** @brief Check if client install routine is invoked for client setup
 */
TEST_F(TestCertificates, InvokeClientInstall)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
}

/** @brief Check if storage install routine is invoked for storage setup
 */
TEST_F(TestCertificates, InvokeAuthorityInstall)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);
    // install the default certificate that's valid from today to 100 years
    // later
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    ASSERT_EQ(certs.size(), 1);
    // check some attributes as well
    EXPECT_EQ(certs.front()->validNotAfter() - certs.front()->validNotBefore(),
              365000ULL * 24 * 3600);
    EXPECT_EQ(certs.front()->subject(), "O=openbmc-project.xyz,CN=localhost");
    EXPECT_EQ(certs.front()->issuer(), "O=openbmc-project.xyz,CN=localhost");

    std::string verifyPath =
        verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";

    // Check that certificate has been created at installation directory
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath));

    // Check that installed cert is identical to input one
    EXPECT_TRUE(compareFiles(certificateFile, verifyPath));
}

/** @brief Check if storage install routine is invoked for storage setup
 */
TEST_F(TestCertificates, InvokeAuthorityInstallNeverExpiredRootCert)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);

    // install the certificate that's valid from the Unix Epoch to Dec 31, 9999
    createNeverExpiredRootCertificate();
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    EXPECT_EQ(certs.front()->validNotBefore(), 0);
    EXPECT_EQ(certs.front()->validNotAfter(), 253402300799ULL);

    std::string verifyPath =
        verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";

    // Check that certificate has been created at installation directory
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath));

    // Check that installed cert is identical to input one
    EXPECT_TRUE(compareFiles(certificateFile, verifyPath));
}

/** @brief Check if in authority mode user can't install the same
 * certificate twice.
 */
TEST_F(TestCertificates, InvokeInstallSameCertTwice)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    EXPECT_FALSE(certs.empty());

    // Check that certificate has been created at installation directory
    std::string verifyPath =
        verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath));

    // Check that installed cert is identical to input one
    EXPECT_TRUE(compareFiles(certificateFile, verifyPath));

    using NotAllowed =
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
    EXPECT_THROW(
        {
            try
            {
                // Try to install the same certificate second time
                mainApp.install(certificateFile);
            }
            catch (const NotAllowed& e)
            {
                throw;
            }
        },
        NotAllowed);

    // Check that the original certificate has been not removed
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath));
}

/** @brief Check if in authority mode user can install a certificate with
 * certain subject hash twice.
 */
TEST_F(TestCertificates, InvokeInstallSameSubjectTwice)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    EXPECT_FALSE(certs.empty());

    // Check that certificate has been created at installation directory
    std::string verifyPath0 =
        verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath0));

    // Check that installed cert is identical to input one
    EXPECT_TRUE(compareFiles(certificateFile, verifyPath0));

    // Prepare second certificate with the same subject
    createNewCertificate();

    // Install second certificate
    mainApp.install(certificateFile);

    // Expect there are exactly two certificates in the collection
    EXPECT_EQ(certs.size(), 2);

    // Check that certificate has been created at installation directory
    std::string verifyPath1 =
        verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".1";
    EXPECT_TRUE(fs::exists(verifyPath1));

    // Check that installed cert is identical to input one
    EXPECT_TRUE(compareFiles(certificateFile, verifyPath1));

    // Check that the original/first certificate has been not removed
    EXPECT_FALSE(fs::is_empty(verifyDir));
    EXPECT_TRUE(fs::exists(verifyPath0));
}

/** @brief Check if in authority mode user can't install more than
 * maxNumAuthorityCertificates certificates.
 */
TEST_F(TestCertificates, InvokeInstallAuthCertLimit)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    std::vector<std::string> verifyPaths;

    // Prepare maximum number of ceritificates
    for (std::size_t i = 0; i < maxNumAuthorityCertificates; ++i)
    {
        // Prepare new certificatate
        createNewCertificate(true);

        // Install ceritificate
        mainApp.install(certificateFile);

        // Check number of certificates in the collection
        EXPECT_EQ(certs.size(), i + 1);

        // Check that certificate has been created at installation directory
        std::string verifyPath =
            verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";
        EXPECT_FALSE(fs::is_empty(verifyDir));
        EXPECT_TRUE(fs::exists(verifyPath));

        // Check that installed cert is identical to input one
        EXPECT_TRUE(compareFiles(certificateFile, verifyPath));

        // Save current certificate file for later check
        verifyPaths.push_back(verifyPath);
    }

    // Prepare new certificatate
    createNewCertificate(true);

    using NotAllowed =
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
    EXPECT_THROW(
        {
            try
            {
                // Try to install one more certificate
                mainApp.install(certificateFile);
            }
            catch (const NotAllowed& e)
            {
                throw;
            }
        },
        NotAllowed);

    // Check that the original certificate has been not removed
    EXPECT_FALSE(fs::is_empty(verifyDir));
    for (size_t i = 0; i < maxNumAuthorityCertificates; ++i)
    {
        EXPECT_TRUE(fs::exists(verifyPaths[i]));
    }
}

/** @brief Compare the installed certificate with the copied certificate
 */
TEST_F(TestCertificates, CompareInstalledCertificate)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
    EXPECT_TRUE(compareFiles(verifyPath, certificateFile));
}

/** @brief Check if install fails if certificate file is not found
 */
TEST_F(TestCertificates, TestNoCertificateFile)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    std::string uploadFile = "nofile.pem";
    EXPECT_THROW(
        {
            try
            {
                auto event = sdeventplus::Event::get_default();
                // Attach the bus to sd_event to service user requests
                bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
                Manager manager(bus, event, objPath.c_str(), type,
                                std::move(unit), std::move(installPath));
                MainApp mainApp(&manager);
                mainApp.install(uploadFile);
            }
            catch (const InternalFailure& e)
            {
                throw;
            }
        },
        InternalFailure);
    EXPECT_FALSE(fs::exists(verifyPath));
}

/** @brief Test replacing existing certificate
 */
TEST_F(TestCertificates, TestReplaceCertificate)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();
    EXPECT_FALSE(certs.empty());
    EXPECT_NE(certs[0], nullptr);
    certs[0]->replace(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
}

/** @brief Test replacing existing certificate
 */
TEST_F(TestCertificates, TestAuthorityReplaceCertificate)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();
    constexpr const unsigned int REPLACE_ITERATIONS = 10;

    for (unsigned int i = 0; i < REPLACE_ITERATIONS; i++)
    {
        // Certificate successfully installed
        EXPECT_FALSE(certs.empty());

        std::string verifyPath =
            verifyDir + "/" + getCertSubjectNameHash(certificateFile) + ".0";

        // Check that certificate has been created at installation directory
        EXPECT_FALSE(fs::is_empty(verifyDir));
        EXPECT_TRUE(fs::exists(verifyPath));

        // Check that installed cert is identical to input one
        EXPECT_TRUE(compareFiles(certificateFile, verifyPath));

        // Create new certificate
        createNewCertificate(true);

        certs[0]->replace(certificateFile);

        // Verify that old certificate has been removed
        EXPECT_FALSE(fs::exists(verifyPath));
    }
}

/** @brief Test verifiing if delete function works.
 */
TEST_F(TestCertificates, TestStorageDeleteCertificate)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Authority;
    std::string verifyDir(certDir);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(certDir));
    MainApp mainApp(&manager);

    // Check if certificate placeholder dir is empty
    EXPECT_TRUE(fs::is_empty(verifyDir));
    mainApp.install(certificateFile);

    // Create new certificate
    createNewCertificate(true);
    mainApp.install(certificateFile);

    createNewCertificate(true);
    mainApp.install(certificateFile);

    std::vector<std::unique_ptr<Certificate>>& certs =
        manager.getCertificates();

    // All 3 certificates successfully installed and added to manager
    EXPECT_EQ(certs.size(), 3);

    // Check if certificate placeholder is not empty, there should be 3
    // certificates
    EXPECT_FALSE(fs::is_empty(verifyDir));

    certs[0]->delete_();
    EXPECT_EQ(certs.size(), 2);

    certs[0]->delete_();
    EXPECT_EQ(certs.size(), 1);

    certs[0]->delete_();
    EXPECT_EQ(certs.size(), 0);

    // Check if certificate placeholder is empty.
    EXPECT_TRUE(fs::is_empty(verifyDir));
}

/** @brief Check if install fails if certificate file is empty
 */
TEST_F(TestCertificates, TestEmptyCertificateFile)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    std::string emptyFile("emptycert.pem");
    std::ofstream ofs;
    ofs.open(emptyFile, std::ofstream::out);
    ofs.close();
    EXPECT_THROW(
        {
            try
            {
                auto event = sdeventplus::Event::get_default();
                // Attach the bus to sd_event to service user requests
                bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
                Manager manager(bus, event, objPath.c_str(), type,
                                std::move(unit), std::move(installPath));
                MainApp mainApp(&manager);
                mainApp.install(emptyFile);
            }
            catch (const InvalidCertificate& e)
            {
                throw;
            }
        },
        InvalidCertificate);
    EXPECT_FALSE(fs::exists(verifyPath));
    fs::remove(emptyFile);
}

/** @brief Check if install fails if certificate file is corrupted
 */
TEST_F(TestCertificates, TestInvalidCertificateFile)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;

    std::ofstream ofs;
    ofs.open(certificateFile, std::ofstream::out);
    ofs << "-----BEGIN CERTIFICATE-----";
    ofs << "ADD_SOME_INVALID_DATA_INTO_FILE";
    ofs << "-----END CERTIFICATE-----";
    ofs.close();

    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    EXPECT_THROW(
        {
            try
            {
                auto event = sdeventplus::Event::get_default();
                // Attach the bus to sd_event to service user requests
                bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
                Manager manager(bus, event, objPath.c_str(), type,
                                std::move(unit), std::move(installPath));
                MainApp mainApp(&manager);
                mainApp.install(certificateFile);
            }
            catch (const InvalidCertificate& e)
            {
                throw;
            }
        },
        InvalidCertificate);
    EXPECT_FALSE(fs::exists(verifyPath));
}

/**
 * Class to generate private and certificate only file and test verification
 */
class TestInvalidCertificate : public ::testing::Test
{
  public:
    TestInvalidCertificate() : bus(sdbusplus::bus::new_default())
    {
    }
    void SetUp() override
    {
        char dirTemplate[] = "/tmp/FakeCerts.XXXXXX";
        auto dirPtr = mkdtemp(dirTemplate);
        if (dirPtr == nullptr)
        {
            throw std::bad_alloc();
        }
        certDir = std::string(dirPtr) + "/certs";
        fs::create_directories(certDir);
        certificateFile = "cert.pem";
        keyFile = "key.pem";
        std::string cmd = "openssl req -x509 -sha256 -newkey rsa:2048 ";
        cmd += "-keyout key.pem -out cert.pem -days 3650 ";
        cmd += "-subj "
               "/O=openbmc-project.xyz/CN=localhost"
               " -nodes";

        auto val = std::system(cmd.c_str());
        if (val)
        {
            std::cout << "command Error: " << val << std::endl;
        }
    }
    void TearDown() override
    {
        fs::remove_all(certDir);
        fs::remove(certificateFile);
        fs::remove(keyFile);
    }

  protected:
    sdbusplus::bus::bus bus;
    std::string certificateFile;
    std::string keyFile;
    std::string certDir;
};

/** @brief Check install fails if private key is missing in certificate file
 */
TEST_F(TestInvalidCertificate, TestMissingPrivateKey)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    EXPECT_THROW(
        {
            try
            {
                auto event = sdeventplus::Event::get_default();
                // Attach the bus to sd_event to service user requests
                bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
                Manager manager(bus, event, objPath.c_str(), type,
                                std::move(unit), std::move(installPath));
                MainApp mainApp(&manager);
                mainApp.install(certificateFile);
            }
            catch (const InternalFailure& e)
            {
                throw;
            }
        },
        InternalFailure);
    EXPECT_FALSE(fs::exists(verifyPath));
}

/** @brief Check install fails if ceritificate is missing in certificate file
 */
TEST_F(TestInvalidCertificate, TestMissingCeritificate)
{
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + keyFile);
    std::string verifyPath(installPath);
    std::string verifyUnit(unit);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    EXPECT_THROW(
        {
            try
            {
                auto event = sdeventplus::Event::get_default();
                // Attach the bus to sd_event to service user requests
                bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
                Manager manager(bus, event, objPath.c_str(), type,
                                std::move(unit), std::move(installPath));
                MainApp mainApp(&manager);
                mainApp.install(keyFile);
            }
            catch (const InternalFailure& e)
            {
                throw;
            }
        },
        InvalidCertificate);
    EXPECT_FALSE(fs::exists(verifyPath));
}

/** @brief Check if error is thrown when multiple certificates are installed
 *  At present only one certificate per service is allowed
 */
TEST_F(TestCertificates, TestCertInstallNotAllowed)
{
    using NotAllowed =
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
    std::string endpoint("ldap");
    std::string unit;
    CertificateType type = CertificateType::Client;
    ;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    MainApp mainApp(&manager);
    mainApp.install(certificateFile);
    EXPECT_TRUE(fs::exists(verifyPath));
    EXPECT_THROW(
        {
            try
            {
                // install second certificate
                mainApp.install(certificateFile);
            }
            catch (const NotAllowed& e)
            {
                throw;
            }
        },
        NotAllowed);
}

TEST_F(TestCertificates, TestGenerateCSR)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("HYB");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("0");
    std::string keyPairAlgorithm("RSA");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    // Attach the bus to sd_event to service user requests
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    std::string csrData("");
    // generateCSR takes considerable time to create CSR and privateKey Files
    EXPECT_FALSE(fs::exists(CSRPath));
    EXPECT_FALSE(fs::exists(privateKeyPath));
    EXPECT_THROW(
        {
            try
            {
                csrData = csr.csr();
            }
            catch (const InternalFailure& e)
            {
                throw;
            }
        },
        InternalFailure);
    // wait for 10 sec to get CSR and privateKey Files generated
    sleep(10);
    EXPECT_TRUE(fs::exists(CSRPath));
    EXPECT_TRUE(fs::exists(privateKeyPath));
    csrData = csr.csr();
    ASSERT_NE("", csrData.c_str());
}

/** @brief Check if ECC key pair is generated when user is not given algorithm
 * type. At present RSA and EC key pair algorithm are supported
 */
TEST_F(TestCertificates, TestGenerateCSRwithEmptyKeyPairAlgorithm)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("HYB");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("");
    std::string keyPairAlgorithm("");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    sleep(10);
    EXPECT_TRUE(fs::exists(CSRPath));
    EXPECT_TRUE(fs::exists(privateKeyPath));
}

/** @brief Check if error is thrown when giving un supported key pair
 * algorithm. At present RSA and EC key pair algorithm are supported
 */
TEST_F(TestCertificates, TestGenerateCSRwithUnsupportedKeyPairAlgorithm)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("HYB");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("secp521r1");
    std::string keyPairAlgorithm("UnSupportedAlgorithm");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    EXPECT_FALSE(fs::exists(CSRPath));
    EXPECT_FALSE(fs::exists(privateKeyPath));
}

/** @brief Check if error is thrown when NID_undef is returned for given key
 * curve id
 */
TEST_F(TestCertificates, TestECKeyGenerationwithNIDundefCase)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("DummyCurveName");
    std::string keyPairAlgorithm("EC");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    EXPECT_FALSE(fs::exists(CSRPath));
    EXPECT_FALSE(fs::exists(privateKeyPath));
}

/** @brief Check default Key Curve Id is used if given curve id is empty
 */
TEST_F(TestCertificates, TestECKeyGenerationwithDefaultKeyCurveId)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("");
    std::string keyPairAlgorithm("EC");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    sleep(10);
    EXPECT_TRUE(fs::exists(CSRPath));
    EXPECT_TRUE(fs::exists(privateKeyPath));
}

/** @brief Check if error is not thrown to generate EC key pair
 */
TEST_F(TestCertificates, TestECKeyGeneration)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("secp521r1");
    std::string keyPairAlgorithm("EC");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    std::cout << "CSRPath: " << CSRPath << std::endl
              << "privateKeyPath: " << privateKeyPath << std::endl;
    sleep(10);
    EXPECT_TRUE(fs::exists(CSRPath));
    EXPECT_TRUE(fs::exists(privateKeyPath));
}

/** @brief Check error is thrown if giving unsupported key bit length to
 * generate rsa key
 */
TEST_F(TestCertificates, TestRSAKeyWithUnsupportedKeyBitLength)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(4096);
    std::string keyCurveId("secp521r1");
    std::string keyPairAlgorithm("RSA");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    EXPECT_FALSE(fs::exists(CSRPath));
    EXPECT_FALSE(fs::exists(privateKeyPath));
}

/** @brief Check error is thrown if generated rsa key file is not present
 */
TEST_F(TestCertificates, TestRSAKeyFileNotPresentCase)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("secp521r1");
    std::string keyPairAlgorithm("RSA");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));

    // Removing generated RSA key file
    fs::remove(rsaPrivateKeyFilePath);

    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    EXPECT_FALSE(fs::exists(CSRPath));
    EXPECT_FALSE(fs::exists(privateKeyPath));
}

/** @brief Check private key file is created from generated rsa key file is
 * `present
 */
TEST_F(TestCertificates, TestRSAKeyFromRSAKeyFileIsWrittenIntoPrivateKeyFile)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    std::string verifyPath(installPath);
    std::string CSRPath(certDir + "/" + CSRFile);
    std::string privateKeyPath(certDir + "/" + privateKeyFile);
    std::vector<std::string> alternativeNames{"localhost1", "localhost2"};
    std::string challengePassword("Password");
    std::string city("BLR");
    std::string commonName("abc.com");
    std::string contactPerson("Admin");
    std::string country("IN");
    std::string email("admin@in.ibm.com");
    std::string givenName("givenName");
    std::string initials("G");
    int64_t keyBitLength(2048);
    std::string keyCurveId("secp521r1");
    std::string keyPairAlgorithm("RSA");
    std::vector<std::string> keyUsage{"serverAuth", "clientAuth"};
    std::string organization("IBM");
    std::string organizationalUnit("orgUnit");
    std::string state("TS");
    std::string surname("surname");
    std::string unstructuredName("unstructuredName");
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    Status status;
    CSR csr(bus, objPath.c_str(), CSRPath.c_str(), status);
    MainApp mainApp(&manager, &csr);
    mainApp.generateCSR(alternativeNames, challengePassword, city, commonName,
                        contactPerson, country, email, givenName, initials,
                        keyBitLength, keyCurveId, keyPairAlgorithm, keyUsage,
                        organization, organizationalUnit, state, surname,
                        unstructuredName);
    sleep(10);
    EXPECT_TRUE(fs::exists(CSRPath));
    EXPECT_TRUE(fs::exists(privateKeyPath));
}

/** @brief Check RSA key is generated during application startup*/
TEST_F(TestCertificates, TestGenerateRSAPrivateKeyFile)
{
    std::string endpoint("https");
    std::string unit;
    CertificateType type = CertificateType::Server;
    std::string installPath(certDir + "/" + certificateFile);
    auto objPath = std::string(objectNamePrefix) + '/' +
                   certificateTypeToString(type) + '/' + endpoint;
    auto event = sdeventplus::Event::get_default();

    EXPECT_FALSE(fs::exists(rsaPrivateKeyFilePath));
    Manager manager(bus, event, objPath.c_str(), type, std::move(unit),
                    std::move(installPath));
    EXPECT_TRUE(fs::exists(rsaPrivateKeyFilePath));
}
} // namespace
} // namespace phosphor::certs
