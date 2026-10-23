# Copyright © 2023 CCP ehf.
import unittest
import collections
import db
import os

import blue
import scheduler


CONNECTION_STRING = os.environ.get('CCP_LOCALDB_CONNECTION_STRING', None)

class Schema:
    def __init__(self):
        self.procedures = {}

    def __getattr__(self, procedureName):
        procedure = self.procedures.get(procedureName)

        if not procedure:
            raise AttributeError(procedureName)

        return procedure

class DbUnitTests(unittest.TestCase):

    def setUp(self):
        if CONNECTION_STRING != None:
            self.session = db.NSession(CONNECTION_STRING)
        print("Set up done")

    def tearDown(self):
        print("Done")

    def _get_schema(self, schema_name):
        procs, tables = self.session.GetSchema(False)
        schemas = collections.defaultdict(Schema)
        for schemaProcName, proc in procs.items():
            if "." in schemaProcName:
                schemaName, procName = schemaProcName.split(".")
                schemas[schemaName].procedures[procName] = schemaProcName

        return schemas[schema_name]

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db") 
    def testGetSchema(self):
        schema = self.session.GetSchema(False)
        self.assertTrue(len(schema) > 0)

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db") 
    def testExcuteProcWithStrArgument(self):
        self.session.allowSync = 1
        # Execute SQL proc
        zsystemSchema = self._get_schema("zsystem")
        zsystemSQLProcName = zsystemSchema.procedures['SQL']
        ret = self.session.Execute(zsystemSQLProcName, ["SELECT a = NULL"])
        self.assertTrue(ret != None)

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db")
    def testExcuteProcWithTwoLongStringResults(self):
        """
        Test that two string keys that should hash the same in the StringStore
        will return the appropriate values.
        """
        self.session.allowSync = 1
        hash_prefix = '1234567890123456789012345678901234567890123456789012345678901234'
        first_value = hash_prefix + "HELLO"
        second_value = hash_prefix + "WORLD"

        zsystemSchema = self._get_schema("zsystem")
        zsystemSQLProcName = zsystemSchema.procedures['SQL']
        SQL_CMD = f"""
            SELECT *
              FROM (VALUES
                     ('{first_value}'),
                     ('{second_value}'))
                AS tmp(s)
        """
        row_set = self.session.Execute(zsystemSQLProcName, [SQL_CMD])[0]
        descriptor, rows = row_set
        first_row, second_row = rows

        self.assertEqual(first_row.s, first_value)
        self.assertEqual(second_row.s, second_value)

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db")
    def testExectutingTaskletCanBeSafelyKilled(self):
        """
        This checks that we don't run into crashes
        due to lifetime issues when a tasklet gets killed.
        """ 
        self.session.allowSync = 1
        procs, tables = self.session.GetSchema(False)
        schemas = collections.defaultdict(Schema)
        for schemaProcName, proc in procs.items():
            if "." in schemaProcName:
                schemaName, procName = schemaProcName.split(".")
                schemas[schemaName].procedures[procName] = schemaProcName

        # Execute SQL proc
        zsystemSchema = schemas['zsystem']
        zsystemSQLProcName = zsystemSchema.procedures['SQL']
        t = scheduler.tasklet(self.session.Execute)
        t(zsystemSQLProcName, ["SELECT a = NULL"])
        t.run()
        t.kill()
        self.assertFalse(t.alive)

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db") 
    def testGetSessionStatus(self):
        sessionsettings = self.session.GetSessionSettings()
        minFreeSessions = sessionsettings['minFreeSessions']
        sessionStatus = self.session.GetSessionStatus()
        self.assertEqual(sessionStatus['sessionCount'], minFreeSessions)
        self.assertEqual(sessionStatus['sessionsInUse'], 0)
        self.assertEqual(sessionStatus['freeSessions'], minFreeSessions)

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db") 
    def testGetSessionSettings(self):
        sessionsettings = self.session.GetSessionSettings()
        self.assertTrue(isinstance(sessionsettings['cleanEvery'], float))
        self.assertTrue(isinstance(sessionsettings['maxSessions'], int))
        self.assertTrue(isinstance(sessionsettings['maxFreeSessions'], int))
        self.assertTrue(isinstance(sessionsettings['minFreeSessions'], int))

    @unittest.skipIf(not CONNECTION_STRING, "Skipping because it requires a local db") 
    def testSetSessionSettings(self):
        #NOTE: Passed in values are not currently type checked
        targetCleanEveryValue = 11.0
        targetMaxSessions = 33
        targetMaxFreeSessions = 9
        targetMinFreeSessions = 3
        self.session.SetSessionSettings({'cleanEvery':targetCleanEveryValue, 'maxSessions':targetMaxSessions, 'maxFreeSessions':targetMaxFreeSessions, 'minFreeSessions':targetMinFreeSessions})

        sessionsettings = self.session.GetSessionSettings()

        self.assertEqual(sessionsettings['cleanEvery'], targetCleanEveryValue)
        self.assertEqual(sessionsettings['maxSessions'], targetMaxSessions)
        self.assertEqual(sessionsettings['maxFreeSessions'], targetMaxFreeSessions)
        self.assertEqual(sessionsettings['minFreeSessions'], targetMinFreeSessions)


def main():
    exc = None

    def wrap_run():
        nonlocal exc
        try:
            unittest.main()
        except Exception as e:
            exc = e

    t = scheduler.tasklet(wrap_run)()
    while t.alive:
        blue.os.Pump()
    if exc:
        raise exc


if __name__ == "__main__":
    main()
